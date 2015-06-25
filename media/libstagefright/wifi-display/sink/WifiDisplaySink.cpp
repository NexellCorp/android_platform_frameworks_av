/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "WifiDisplaySink"
#include <utils/Log.h>

#include "WifiDisplaySink.h"

#include "DirectRenderer.h"
#include "MediaReceiver.h"
#include "TimeSyncer.h"

 // by sapark
 #include "Parameters.h"

#include <cutils/properties.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ParsedMessage.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>

namespace android {

// static
const AString WifiDisplaySink::sUserAgent = MakeUserAgent();

// by sapark uibd thread
void * doUibcLoop(void *data);
void doMakeUibcEvent(struct input_event2 * event);
void doSendUibcEvent(sp<WifiDisplaySink> sink, struct input_event2 * event);

WifiDisplaySink::WifiDisplaySink(
        uint32_t flags,
        const sp<ANetworkSession> &netSession,
        const sp<IGraphicBufferProducer> &bufferProducer,
        const sp<AMessage> &notify)
    : mState(UNDEFINED),
      mFlags(flags),
      mNetSession(netSession),
      mSurfaceTex(bufferProducer),
      mNotify(notify),
      mUsingTCPTransport(false),
      mUsingTCPInterleaving(false),
      mSessionID(0),
      mNextCSeq(1),
      mIDRFrameRequestPending(false),
      mTimeOffsetUs(0ll),
      mTimeOffsetValid(false),
      mSetupDeferred(false),
      mLatencyCount(0),
      mLatencySumUs(0ll),
      mLatencyMaxUs(0ll),
      mMaxDelayMs(-1ll) {

    	// We support any and all resolutions, but prefer 720p30

	char val[PROPERTY_VALUE_MAX];
	int high_profile = 0;	
	if (property_get("media.wfd-sink.wfd-format", val, NULL)) {	
		 if (!strcasecmp("1080", val)  ) {
		 	high_profile = 1;
			ALOGI("**media.wfd-sink.wfd-format : 1080 *************************");
		 }
	} 

    high_profile = 1;

	if( high_profile ) {
		mSinkSupportedVideoFormats.setNativeResolution(VideoFormats::RESOLUTION_CEA, 5);  // 1280 x 720 p30
    		mSinkSupportedVideoFormats.enableAll();
	} else {
		mSinkSupportedVideoFormats.disableAll();
	    	mSinkSupportedVideoFormats.setNativeResolution(VideoFormats::RESOLUTION_CEA, 5);  // 1280 x 720 p30
	}

	
    // by sapark
    mFindUibcPort = false;
    mUibcPort = 0x00;
    mUibcSessionID = 0x00;
    mUibcThread = 0x00;
    bTerminatedUibc = true;

    mMediaReceiverLooper = NULL;
    mMediaReceiver = NULL;

    mWindowSize = false;
    mWidth = 0x00;
    mHeight = 0x00;

}

WifiDisplaySink::~WifiDisplaySink() {
    ALOGD("########## WifiDisplaySink::~WifiDisplaySink()");
}

void WifiDisplaySink::start(const char *sourceHost, int32_t sourcePort) {
    sp<AMessage> msg = new AMessage(kWhatStart, id());
    msg->setString("sourceHost", sourceHost);
    msg->setInt32("sourcePort", sourcePort);
    msg->post();
}

void WifiDisplaySink::start(const char *uri) {
    sp<AMessage> msg = new AMessage(kWhatStart, id());
    msg->setString("setupURI", uri);
    msg->post();
}

// by saprk 
void WifiDisplaySink::stop() {

    if (mMediaReceiverLooper != NULL) {
        mMediaReceiverLooper->stop();
        mMediaReceiverLooper.clear();
    }

    if( mSessionID > 0 ) {
        mNetSession->destroySession(mSessionID);
        mSessionID = 0;
    }

    if(mUibcSessionID>0) {
        bTerminatedUibc = true;
        mNetSession->destroySession(mUibcSessionID);
        mUibcSessionID = 0x00;
    }
 
    if (mNotify == NULL) {
        looper()->stop();
    } else {
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatDisconnected);
        notify->post();
    }

}

// static
bool WifiDisplaySink::ParseURL(
        const char *url, AString *host, int32_t *port, AString *path,
        AString *user, AString *pass) {
    host->clear();
    *port = 0;
    path->clear();
    user->clear();
    pass->clear();

    if (strncasecmp("rtsp://", url, 7)) {
        return false;
    }

    const char *slashPos = strchr(&url[7], '/');

    if (slashPos == NULL) {
        host->setTo(&url[7]);
        path->setTo("/");
    } else {
        host->setTo(&url[7], slashPos - &url[7]);
        path->setTo(slashPos);
    }

    ssize_t atPos = host->find("@");

    if (atPos >= 0) {
        // Split of user:pass@ from hostname.

        AString userPass(*host, 0, atPos);
        host->erase(0, atPos + 1);

        ssize_t colonPos = userPass.find(":");

        if (colonPos < 0) {
            *user = userPass;
        } else {
            user->setTo(userPass, 0, colonPos);
            pass->setTo(userPass, colonPos + 1, userPass.size() - colonPos - 1);
        }
    }

    const char *colonPos = strchr(host->c_str(), ':');

    if (colonPos != NULL) {
        char *end;
        unsigned long x = strtoul(colonPos + 1, &end, 10);

        if (end == colonPos + 1 || *end != '\0' || x >= 65536) {
            return false;
        }

        *port = x;

        size_t colonOffset = colonPos - host->c_str();
        size_t trailing = host->size() - colonOffset;
        host->erase(colonOffset, trailing);
    } else {
        *port = 554;
    }

    return true;
}

void WifiDisplaySink::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatStart:
        {
            sleep(2);  // XXX

            int32_t sourcePort;
            CHECK(msg->findString("sourceHost", &mRTSPHost));
            CHECK(msg->findInt32("sourcePort", &sourcePort));

            sp<AMessage> notify = new AMessage(kWhatRTSPNotify, id());

            status_t err = mNetSession->createRTSPClient(
                    mRTSPHost.c_str(), sourcePort, notify, &mSessionID);
            CHECK_EQ(err, (status_t)OK);
			
			
			ALOGE("******************************** sapark42 **********************************");

            mState = CONNECTING;
            break;
        }

        case kWhatRTSPNotify:
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));

            switch (reason) {
                case ANetworkSession::kWhatError:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    ALOGE("An error occurred in session %d (%d, '%s/%s').",
                          sessionID,
                          err,
                          detail.c_str(),
                          strerror(-err));

                    if (sessionID == mSessionID) {
                        ALOGI("Lost control connection.");

                        // The control connection is dead now.
                        // by sapark
                        stop();
                        // mNetSession->destroySession(mSessionID);
                        // mSessionID = 0;

                        // // by sapark
                        // if (mUibcSessionID > 0x00){
                        //     ALOGI("######################## kWhatRTSPNotify::uibc destory");
                        //     bTerminatedUibc = true;
                        //     mNetSession->destroySession(mUibcSessionID);
                        //     mUibcSessionID = 0;
                        // }

                        // if (mNotify == NULL) {
                        //     looper()->stop();
                        // } else {
                        //     sp<AMessage> notify = mNotify->dup();
                        //     notify->setInt32("what", kWhatDisconnected);
                        //     notify->post();
                        // }
                    }
                    break;
                }

                case ANetworkSession::kWhatConnected:
                {
                    ALOGI("We're now connected.");
                    mState = CONNECTED;

                    if (mFlags & FLAG_SPECIAL_MODE) {
                        sp<AMessage> notify = new AMessage(
                                kWhatTimeSyncerNotify, id());

                        mTimeSyncer = new TimeSyncer(mNetSession, notify);
                        looper()->registerHandler(mTimeSyncer);

                        mTimeSyncer->startClient(mRTSPHost.c_str(), 8123);
                    }
                    break;
                }

                case ANetworkSession::kWhatData:
                {
                    onReceiveClientData(msg);
                    break;
                }

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatStop:
        {
            // by sapark
            //looper()->stop();
            stop();
            break;
        }

        case kWhatMediaReceiverNotify:
        {
            onMediaReceiverNotify(msg);
            break;
        }

        case kWhatTimeSyncerNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == TimeSyncer::kWhatTimeOffset) {
                CHECK(msg->findInt64("offset", &mTimeOffsetUs));
                mTimeOffsetValid = true;

                if (mSetupDeferred) {
                    CHECK_EQ((status_t)OK,
                             sendSetup(
                                mSessionID,
                                "rtsp://x.x.x.x:x/wfd1.0/streamid=0"));

                    mSetupDeferred = false;
                }
            }
            break;
        }

        case kWhatReportLateness:
        {
            if (mLatencyCount > 0) {
                int64_t avgLatencyUs = mLatencySumUs / mLatencyCount;

                ALOGV("avg. latency = %lld ms (max %lld ms)",
                      avgLatencyUs / 1000ll,
                      mLatencyMaxUs / 1000ll);

                sp<AMessage> params = new AMessage;
                params->setInt64("avgLatencyUs", avgLatencyUs);
                params->setInt64("maxLatencyUs", mLatencyMaxUs);
                mMediaReceiver->informSender(0 /* trackIndex */, params);
            }

            mLatencyCount = 0;
            mLatencySumUs = 0ll;
            mLatencyMaxUs = 0ll;

            msg->post(kReportLatenessEveryUs);
            break;
        }

        case kWhatUibcNotify:
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));
            ALOGD("################################ kWhatUibcNotify:reason:%d", reason);
            switch (reason) {
                case ANetworkSession::kWhatError:
                {
                    ALOGE("################################ kWhatUibcNotify:kWhatError");

                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    ALOGE("################################ kWhatUibcNotify:kWhatError");
                    ALOGE("session:%d, err:%d, detail:%s", sessionID, err, detail.c_str(),strerror(-err));

                    if (sessionID == mUibcSessionID) {
                        ALOGE("################################ kWhatUibcNotify:kWhatError: lost connection");
                        // The control connection is dead now.
                        bTerminatedUibc = true;
                        mNetSession->destroySession(mUibcSessionID);
                        mUibcSessionID = 0;
                    }
                    break;
                }

                case ANetworkSession::kWhatConnected:
                {
                    ALOGD("################################ kWhatUIBCNotify:kWhatConnected 333");
                    // unsigned char d[12] = {0x00,0x00,0x00,0x0c,0x03,0x00,0x05,0x00,0x00,0x07,0x00,0x00};
                    // // unsigned char d[14] = {0x10,0x00,0x00,0x0a,0x00,0x00,0x03,0x00,0x05,0x00,0x00,0x07,0x00,0x00};
                    // mNetSession->sendRequest(mUibcSessionID, d, 12, false, -1ll, true);

                    // ALOGD("################################ kWhatUIBCNotify:kWhatConnected 444");
                    // unsigned char f[12] = {0x00,0x00,0x00,0x0c,0x04,0x00,0x05,0x00,0x00,0x07,0x00,0x00};
                    // // unsigned char f[14] = {0x10,0x00,0x00,0x0a,0x00,0x00,0x04,0x00,0x05,0x00,0x00,0x07,0x00,0x00};
                    // mNetSession->sendRequest(mUibcSessionID, f, 12);
                    break;
                }

                case ANetworkSession::kWhatData:
                {
                    ALOGD("################################ kWhatUIBCNotify:kWhatData");
                    break;
                }
                default:
                    TRESPASS();
            }
            break;
        }
        default:
            TRESPASS();
    }
}

void WifiDisplaySink::dumpDelay(size_t trackIndex, int64_t timeUs) {
    int64_t delayMs = (ALooper::GetNowUs() - timeUs) / 1000ll;

    if (delayMs > mMaxDelayMs) {
        mMaxDelayMs = delayMs;
    }

    static const int64_t kMinDelayMs = 0;
    static const int64_t kMaxDelayMs = 300;

    const char *kPattern = "########################################";
    size_t kPatternSize = strlen(kPattern);

    int n = (kPatternSize * (delayMs - kMinDelayMs))
                / (kMaxDelayMs - kMinDelayMs);

    if (n < 0) {
        n = 0;
    } else if ((size_t)n > kPatternSize) {
        n = kPatternSize;
    }

    ALOGI("[%lld]: (%4lld ms / %4lld ms) %s",
          timeUs / 1000,
          delayMs,
          mMaxDelayMs,
          kPattern + kPatternSize - n);
}

void WifiDisplaySink::onMediaReceiverNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case MediaReceiver::kWhatInitDone:
        {
            status_t err;
            CHECK(msg->findInt32("err", &err));

            ALOGI("MediaReceiver initialization completed w/ err %d", err);
            break;
        }

        case MediaReceiver::kWhatError:
        {
            status_t err;
            CHECK(msg->findInt32("err", &err));

            ALOGE("MediaReceiver signaled error %d", err);
            break;
        }

        case MediaReceiver::kWhatAccessUnit:
        {
            ALOGV("%s: get kWhatAccessUnit", __func__);
            if (mRenderer == NULL) {
                mRenderer = new DirectRenderer(mSurfaceTex);
                looper()->registerHandler(mRenderer);
            }

            sp<ABuffer> accessUnit;
            CHECK(msg->findBuffer("accessUnit", &accessUnit));

            int64_t timeUs;
            CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

            if (!mTimeOffsetValid && !(mFlags & FLAG_SPECIAL_MODE)) {
                mTimeOffsetUs = timeUs - ALooper::GetNowUs();
                mTimeOffsetValid = true;
            }

            CHECK(mTimeOffsetValid);

            // We are the timesync _client_,
            // client time = server time - time offset.
            timeUs -= mTimeOffsetUs;

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            int64_t nowUs = ALooper::GetNowUs();
            int64_t delayUs = nowUs - timeUs;

            mLatencySumUs += delayUs;
            if (mLatencyCount == 0 || delayUs > mLatencyMaxUs) {
                mLatencyMaxUs = delayUs;
            }
            ++mLatencyCount;

            // dumpDelay(trackIndex, timeUs);

            timeUs += 220000ll;  // Assume 220 ms of latency
            accessUnit->meta()->setInt64("timeUs", timeUs);

            sp<AMessage> format;
            if (msg->findMessage("format", &format)) {
                mRenderer->setFormat(trackIndex, format);
            }

            mRenderer->queueAccessUnit(trackIndex, accessUnit);
            break;
        }

        case MediaReceiver::kWhatPacketLost:
        {
#if 0
            if (!mIDRFrameRequestPending) {
                ALOGI("requesting IDR frame");

                sendIDRFrameRequest(mSessionID);
            }
#endif
            break;
        }

        default:
            TRESPASS();
    }
}

void WifiDisplaySink::registerResponseHandler(
        int32_t sessionID, int32_t cseq, HandleRTSPResponseFunc func) {
    ResponseID id;
    id.mSessionID = sessionID;
    id.mCSeq = cseq;
    mResponseHandlers.add(id, func);
}

status_t WifiDisplaySink::sendM2(int32_t sessionID) {
    AString request = "OPTIONS * RTSP/1.0\r\n";
    AppendCommonResponse(&request, mNextCSeq);

    request.append(
            "Require: org.wfa.wfd1.0\r\n"
            "\r\n");

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceiveM2Response);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::onReceiveM2Response(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    return OK;
}

status_t WifiDisplaySink::onReceiveSetupResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    if (!msg->findString("session", &mPlaybackSessionID)) {
        return ERROR_MALFORMED;
    }

    if (!ParsedMessage::GetInt32Attribute(
                mPlaybackSessionID.c_str(),
                "timeout",
                &mPlaybackSessionTimeoutSecs)) {
        mPlaybackSessionTimeoutSecs = -1;
    }

    ssize_t colonPos = mPlaybackSessionID.find(";");
    if (colonPos >= 0) {
        // Strip any options from the returned session id.
        mPlaybackSessionID.erase(
                colonPos, mPlaybackSessionID.size() - colonPos);
    }

    status_t err = configureTransport(msg);

    if (err != OK) {

        //dsyou <-- server missing part°¡ ÀÖ´Â ³ðµé..
        //return err;
    }

    mState = PAUSED;

    return sendPlay(
            sessionID,
            "rtsp://x.x.x.x:x/wfd1.0/streamid=0");
}

status_t WifiDisplaySink::configureTransport(const sp<ParsedMessage> &msg) {
    if (mUsingTCPTransport && !(mFlags & FLAG_SPECIAL_MODE)) {
        // In "special" mode we still use a UDP RTCP back-channel that
        // needs connecting.
        return OK;
    }

    AString transport;
    if (!msg->findString("transport", &transport)) {
        ALOGE("Missing 'transport' field in SETUP response.");
        return ERROR_MALFORMED;
    }

    AString sourceHost;
    if (!ParsedMessage::GetAttribute(
                transport.c_str(), "source", &sourceHost)) {
        sourceHost = mRTSPHost;
    }

    AString serverPortStr;
    if (!ParsedMessage::GetAttribute(
                transport.c_str(), "server_port", &serverPortStr)) {
        ALOGE("Missing 'server_port' in Transport field.");
        return ERROR_MALFORMED;
    }

    int rtpPort, rtcpPort;

#if 1
    //dsyou Galaxy Tab 10.1
    sscanf(serverPortStr.c_str(), "%d", &rtpPort );
    rtcpPort = rtpPort +1;
#else
    if (sscanf(serverPortStr.c_str(), "%d-%d", &rtpPort, &rtcpPort) != 2
            || rtpPort <= 0 || rtpPort > 65535
            || rtcpPort <=0 || rtcpPort > 65535
            || rtcpPort != rtpPort + 1) {
        ALOGE("Invalid server_port description '%s'.",
                serverPortStr.c_str());

        return ERROR_MALFORMED;
    }
#endif

    if (rtpPort & 1) {
        ALOGW("Server picked an odd numbered RTP port.");
    }

    ALOGD("****** sapark : rtp(%d), rtcp(%d)", rtpPort, rtcpPort);

    return mMediaReceiver->connectTrack(
            0 /* trackIndex */, sourceHost.c_str(), rtpPort, rtcpPort);
}

status_t WifiDisplaySink::onReceivePlayResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        ALOGE("onReceivePlayResponse: error line %d", __LINE__);
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        ALOGE("onReceivePlayResponse: error line %d", __LINE__);
        return ERROR_UNSUPPORTED;
    }

    // psw0523 test
#if 0
    AString session;
    if (!msg->findString("session", &session)) {
        ALOGE("onReceivePlayResponse can't find session, retry PLAY Request");
        sendIDRFrameRequest(sessionID);
        //return OK;
    }
#endif

    ALOGD("START Playing =====> ");
    mState = PLAYING;

    // psw0523 test
    (new AMessage(kWhatReportLateness, id()))->post(kReportLatenessEveryUs);

    return OK;
}

status_t WifiDisplaySink::onReceiveIDRFrameRequestResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    CHECK(mIDRFrameRequestPending);
    mIDRFrameRequestPending = false;

    return OK;
}

void WifiDisplaySink::onReceiveClientData(const sp<AMessage> &msg) {
    int32_t sessionID;
    CHECK(msg->findInt32("sessionID", &sessionID));

    sp<RefBase> obj;
    CHECK(msg->findObject("data", &obj));

    sp<ParsedMessage> data =
        static_cast<ParsedMessage *>(obj.get());

    ALOGD("REQUEST =======> session %d received '%s'",
          sessionID, data->debugString().c_str());

    AString method;
    AString uri;
    data->getRequestField(0, &method);

    int32_t cseq;
    if (!data->findInt32("cseq", &cseq)) {
        sendErrorResponse(sessionID, "400 Bad Request", -1 /* cseq */);
        return;
    }

    if (method.startsWith("RTSP/")) {
        // This is a response.

        ResponseID id;
        id.mSessionID = sessionID;
        id.mCSeq = cseq;

        ssize_t index = mResponseHandlers.indexOfKey(id);

        if (index < 0) {
            ALOGW("Received unsolicited server response, cseq %d", cseq);
            return;
        }

        HandleRTSPResponseFunc func = mResponseHandlers.valueAt(index);
        mResponseHandlers.removeItemsAt(index);

        status_t err = (this->*func)(sessionID, data);
        CHECK_EQ(err, (status_t)OK);
    } else {
        AString version;
        data->getRequestField(2, &version);
        if (!(version == AString("RTSP/1.0"))) {
            sendErrorResponse(sessionID, "505 RTSP Version not supported", cseq);
            return;
        }

        if (method == "OPTIONS") {
            onOptionsRequest(sessionID, cseq, data);
        } else if (method == "GET_PARAMETER") {
            onGetParameterRequest(sessionID, cseq, data);
        } else if (method == "SET_PARAMETER") {
            onSetParameterRequest(sessionID, cseq, data);
        } else {
            sendErrorResponse(sessionID, "405 Method Not Allowed", cseq);
        }
    }
}

void WifiDisplaySink::onOptionsRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n");
    response.append("\r\n");

    ALOGD("RESPONSE ===========>");
    ALOGD("%s", response.c_str());
    ALOGD("<===========");
    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);

    err = sendM2(sessionID);
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySink::onGetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    AString body;

    if (mState == CONNECTED) {
        mUsingTCPTransport = false;
        mUsingTCPInterleaving = false;

        char val[PROPERTY_VALUE_MAX];
        if (property_get("media.wfd-sink.tcp-mode", val, NULL)) {
            if (!strcasecmp("true", val) || !strcmp("1", val)) {
                ALOGI("Using TCP unicast transport.");
                mUsingTCPTransport = true;
                mUsingTCPInterleaving = false;
            } else if (!strcasecmp("interleaved", val)) {
                ALOGI("Using TCP interleaved transport.");
                mUsingTCPTransport = true;
                mUsingTCPInterleaving = true;
            }
        } else if (mFlags & FLAG_SPECIAL_MODE) {
            mUsingTCPTransport = true;
        }

        body = "wfd_video_formats: ";
        body.append(mSinkSupportedVideoFormats.getFormatSpec());

        // psw0523 add
#if 0
        ALOGD("add wfd_content_protection");
        body.append(
                 "\r\nwfd_content_protection: none");
        ALOGD("add wfd_coupled_sink");
        body.append(
                 "\r\nwfd_coupled_sink: none");
        ALOGD("add wfd_uibc_capability");
        body.append(
                 "\r\nwfd_uibc_capability: none");
        //ALOGD("add wfd_standby_resume_capablility");
        //body.append(
                 //"\r\nwfd_standby_resume_capablility: none");
#endif
        // end psw0523

        // by sapark : for uibc
//wfd_uibc_capability: input_category_list=GENERIC, HIDC;generic_cap_list=Keyboard, SingleTouch, MultiTouch;hidc_cap_list=none;port=5000
        body.append("\r\nwfd_uibc_capability: input_category_list=GENERIC;generic_cap_list=Keyboard, Mouse, SingleTouch, MultiTouch;hidc_cap_list=none;port=7239");

        body.append(
                "\r\nwfd_audio_codecs: AAC 0000000F 00\r\n"
                "wfd_client_rtp_ports: RTP/AVP/");


        if (mUsingTCPTransport) {
            body.append("TCP;");
            if (mUsingTCPInterleaving) {
                body.append("interleaved");
            } else {
                body.append("unicast 19000 0");
            }
        } else {
            body.append("UDP;unicast 19000 0");
        }

        body.append(" mode=play\r\n");
    }

    // by sapark for test
    // if (mState == PLAYING && mUibcSessionID > 0x00) {
    //     ALOGD("########### onGetParameterRequest:send UIBC");
    //     unsigned char d[12] = {0x00,0x00,0x00,0x0c,0x03,0x00,0x05,0x00,0x00,0x07,0x00,0x00};
    //     status_t err = mNetSession->sendRequest(mUibcSessionID, d, 12, false, -1ll, true);
    //     if(err != OK){
    //         ALOGE("########### onGetParameterRequest:send UIBC : ERROR: %d", err);
    //     }else{
    //         ALOGE("########### onGetParameterRequest:send UIBC : SUCCESS: %d", err);
    //     }

    //     // ALOGD("########### onGetParameterRequest:send UIBC2");
    //     // unsigned char f[12] = {0x00,0x00,0x00,0x0c,0x04,0x00,0x05,0x00,0x00,0x07,0x00,0x00};
    //     // err = mNetSession->sendRequest(mUibcSessionID, f, 12);
    //     // if(err != OK){
    //     //     ALOGE("########### onGetParameterRequest:send UIBC2 : ERROR: %d", err);
    //     // }else{
    //     //     ALOGE("########### onGetParameterRequest:send UIBC2 : SUCCESS: %d", err);
    //     // }


    //     // int x = rand()%720;
    //     // int y = rand()%480;
    //     // ALOGD("########### onGetParameterRequest:send UIBC touch x:0x%08x, y:0x%08x", x, y);
    //     // unsigned char g[13] = {0x00,0x00,0x00,0x09,0x00,0x00,0x06,0x01,0x00,((x >> 8)&0xFF),(x & 0xFF),((y >> 8)&0xFF),(y & 0xFF)};
    //     // ALOGD("########### onGetParameterRequest:send UIBC touch x1:0x%02x, x2:0x%02x, y1:0x%02x, y2:0x%02x,", g[9], g[10], g[11], g[12]);
    //     // mNetSession->sendRequest(mUibcSessionID, f, 13);
    //     // ALOGD("########### onGetParameterRequest:send UIBC touch2");
    //     // unsigned char h[13] = {0x00,0x00,0x00,0x09,0x01,0x00,0x06,0x01,0x00,((x >> 8)&0xFF),(x & 0xFF),((y >> 8)&0xFF),(y & 0xFF)};
    //     // mNetSession->sendRequest(mUibcSessionID, h, 13);
    // }

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("Content-Type: text/parameters\r\n");
    response.append(StringPrintf("Content-Length: %d\r\n", body.size()));
    response.append("\r\n");
    response.append(body);

    ALOGD("RESPONSE ===========>");
    ALOGD("%s", response.c_str());
    ALOGD("<===========");
    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

status_t WifiDisplaySink::sendSetup(int32_t sessionID, const char *uri) {

    sp<AMessage> notify = new AMessage(kWhatMediaReceiverNotify, id());

    mMediaReceiverLooper = new ALooper;
    mMediaReceiverLooper->setName("media_receiver");

    mMediaReceiverLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    mMediaReceiver = new MediaReceiver(mNetSession, notify);
    mMediaReceiverLooper->registerHandler(mMediaReceiver);

    RTPReceiver::TransportMode rtpMode = RTPReceiver::TRANSPORT_UDP;
    if (mUsingTCPTransport) {
        if (mUsingTCPInterleaving) {
            rtpMode = RTPReceiver::TRANSPORT_TCP_INTERLEAVED;
        } else {
            rtpMode = RTPReceiver::TRANSPORT_TCP;
        }
    }

    int32_t localRTPPort;
    status_t err = mMediaReceiver->addTrack(
            rtpMode, RTPReceiver::TRANSPORT_UDP /* rtcpMode */, &localRTPPort);

    if (err == OK) {
        err = mMediaReceiver->initAsync(MediaReceiver::MODE_TRANSPORT_STREAM);
    }

    if (err != OK) {
        mMediaReceiverLooper->unregisterHandler(mMediaReceiver->id());
        mMediaReceiver.clear();

        mMediaReceiverLooper->stop();
        mMediaReceiverLooper.clear();

        mMediaReceiverLooper = NULL;
        mMediaReceiver = NULL;
        return err;
    }

    AString request = StringPrintf("SETUP %s RTSP/1.0\r\n", uri);

    AppendCommonResponse(&request, mNextCSeq);

    if (rtpMode == RTPReceiver::TRANSPORT_TCP_INTERLEAVED) {
        request.append("Transport: RTP/AVP/TCP;interleaved=0-1\r\n");
    } else if (rtpMode == RTPReceiver::TRANSPORT_TCP) {
        if (mFlags & FLAG_SPECIAL_MODE) {
            // This isn't quite true, since the RTP connection is through TCP
            // and the RTCP connection through UDP...
            request.append(
                    StringPrintf(
                        "Transport: RTP/AVP/TCP;unicast;client_port=%d-%d\r\n",
                        localRTPPort, localRTPPort + 1));
        } else {
            request.append(
                    StringPrintf(
                        "Transport: RTP/AVP/TCP;unicast;client_port=%d\r\n",
                        localRTPPort));
        }
    } else {


#if 1
        request.append(
                StringPrintf(
                    "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n",
                    localRTPPort,
                    localRTPPort + 1));

#else
        request.append(
                StringPrintf(
                    "Transport: RTP/AVP/UDP;unicast;client_port=%d\r\n",
                    localRTPPort));
#endif
              
    }

    request.append("\r\n");

    ALOGD("RESPONSE ===========>");
    ALOGD("%s", request.c_str());
    ALOGD("<===========");
    //ALOGV("request = '%s'", request.c_str());

    err = mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceiveSetupResponse);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::sendPlay(int32_t sessionID, const char *uri) {
    AString request = StringPrintf("PLAY %s RTSP/1.0\r\n", uri);

    AppendCommonResponse(&request, mNextCSeq);

    // psw0523 add
    //if (mPlaybackSessionID == "00000000") {
        //mPlaybackSessionID.setTo("1517089259");
    //}
    // end psw0523

    request.append(StringPrintf("Session: %s\r\n", mPlaybackSessionID.c_str()));
    request.append("\r\n");

    ALOGD("RESPONSE ===========>");
    ALOGD("%s", request.c_str());
    ALOGD("<===========");

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceivePlayResponse);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::sendIDRFrameRequest(int32_t sessionID) {
    CHECK(!mIDRFrameRequestPending);

    AString request = "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n";

    AppendCommonResponse(&request, mNextCSeq);

    AString content = "wfd_idr_request\r\n";

    request.append(StringPrintf("Session: %s\r\n", mPlaybackSessionID.c_str()));
    request.append(StringPrintf("Content-Length: %d\r\n", content.size()));
    request.append("\r\n");
    request.append(content);

    ALOGD("RESPONSE ===========>");
    ALOGD("%s", request.c_str());
    ALOGD("<===========");

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID,
            mNextCSeq,
            &WifiDisplaySink::onReceiveIDRFrameRequestResponse);

    ++mNextCSeq;

    mIDRFrameRequestPending = true;

    return OK;
}

void WifiDisplaySink::onSetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    const char *content = data->getContent();
    if (strstr(content, "wfd_trigger_method: SETUP\r\n") != NULL) {
        if ((mFlags & FLAG_SPECIAL_MODE) && !mTimeOffsetValid) {
            mSetupDeferred = true;
        } else {
            status_t err =
                sendSetup(
                        sessionID,
                        "rtsp://x.x.x.x:x/wfd1.0/streamid=0");

            CHECK_EQ(err, (status_t)OK);
        }
    }

    // by sapark

    sp<Parameters> params = Parameters::Parse(data->getContent(), strlen(data->getContent()));

    if (params != NULL) {
        AString format;
        if (params->findParameter("wfd_video_formats", &format)) {
            ALOGD("########## onSetParameterRequest:wfd_video_formats: find");
            ALOGD("%s", format.c_str());

            VideoFormats mSourceSupportedVideoFormats;    // by sapark
            bool ret = mSourceSupportedVideoFormats.parseFormatSpec(format.c_str());
            if (ret) {

                // VideoFormats::ResolutionType type;
                // size_t index;
                for(int i = 0 ; i < 32 ; i++) {
                    bool enable = mSourceSupportedVideoFormats.isResolutionEnabled(VideoFormats::RESOLUTION_CEA, i);
                    if (enable) {
                        ALOGD("########## onSetParameterRequest:wfd_video_formats: index:%d", i);
                        size_t width, height;
			   
                        bool ret = mSourceSupportedVideoFormats.GetConfiguration(VideoFormats::RESOLUTION_CEA, i, &width, &height, NULL, NULL);
                        if (ret) {
                            ALOGD("########## onSetParameterRequest:wfd_video_formats: w:%d, h:%d", width, height);
                            mWindowSize = true;
                            mWidth = width;
                            mHeight = height;
                        }
			   
                        break;
                    }
                }
		  
               // mSourceSupportedVideoFormats.getNativeResolution(&type, &index);
                // ALOGD("########## onSetParameterRequest:wfd_video_formats: type:%d, index:%d", type, index);
            }
            else {
                ALOGE("########## onSetParameterRequest:wfd_video_formats: parse error");
            }

        }
    }
	
    // const char * format = strstr(content, "wfd_video_formats: ");
    // if (format != NULL) {
    //     ALOGD("########## onSetParameterRequest:wfd_video_formats: find");
    //     ALOGD("%s", format);

    //     VideoFormats mSourceSupportedVideoFormats;    // by sapark
    //     bool ret = mSourceSupportedVideoFormats.parseFormatSpec(format);
    //     if (ret) {
    //         ALOGD("########## onSetParameterRequest:wfd_video_formats: parse success");
    //         // VideoFormats::ResolutionType type;
    //         // size_t index;
    //         // mSourceSupportedVideoFormats.getNativeResolution(&type, &index);
    //         // ALOGD("########## onSetParameterRequest:wfd_video_formats: type:%d, index:%d", type, index);
    //     }
    //     else {
    //         ALOGE("########## onSetParameterRequest:wfd_video_formats: parse error");
    //     }
    // }


    const char * capability = strstr(content, "wfd_uibc_capability:");
    if (capability != NULL) {
        ALOGD("########## onSetParameterRequest:wfd_uibc_capability find");
        ALOGD("%s", capability);
        AString port;
        mFindUibcPort = true;
        mUibcPort = 7239;
        if(ParsedMessage::GetAttribute(capability, "port", &port)){
            ALOGD("########## onSetParameterRequest:wfd_uibc_capability port:%s", port.c_str());
            sscanf(port.c_str(), "%d", &mUibcPort);
        }
        ALOGD("########## onSetParameterRequest:wfd_uibc_capability complete port:%d", mUibcPort);
    }
    // mUibcPort = 7239;
    if (strstr(content, "wfd_uibc_setting: enable\r\n") != NULL) {
        ALOGD("########## onSetParameterRequest:wfd_uibc_setting: enable :%d\r\n", mFindUibcPort);
        if(mFindUibcPort){
            ALOGD(">> host:%s, port:%d", mRTSPHost.c_str(), mUibcPort);

            int32_t session = 0x00;
            sp<AMessage> notify = new AMessage(kWhatUibcNotify, id());
            status_t err = mNetSession->createTCPDatagramSession(
                7239,   // sink port
                mRTSPHost.c_str(),
                mUibcPort,
                notify,
                &session);



            if (err != OK) {
                ALOGE("######### UIBC network session create fail");
                bTerminatedUibc = true;
                mNetSession->destroySession(session);
                mUibcSessionID = 0x00;
            } else {

                ALOGD("########## onSetParameterRequest:wfd_uibc_setting: create thread");
                bTerminatedUibc = false;
                pthread_create(&mUibcThread, NULL, doUibcLoop, (void *)this);
                ALOGD("########## onSetParameterRequest:wfd_uibc_setting: created %d", mUibcThread);
                mUibcSessionID = session;
            }

            ALOGD("########## onSetParameterRequest:wfd_uibc_setting: ret: %d", mUibcSessionID);
        }
    }

    if (strstr(content, "wfd_uibc_setting: disable\r\n") != NULL) {
        ALOGD("########## onSetParameterRequest:wfd_uibc_setting: disable\r\n");
        if(mUibcSessionID>0) {
            ALOGD("########## onSetParameterRequest: destroysession");
            bTerminatedUibc = true;
            mNetSession->destroySession(mUibcSessionID);
            mUibcSessionID = 0x00;
        }
    }

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("\r\n");

    ALOGD("RESPONSE ===========>");
    ALOGD("%s", response.c_str());
    ALOGD("<===========");
    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);


    // lesc0.150527
    if (strstr(content, "wfd_trigger_method: TEARDOWN\r\n") != NULL) 
    {
        ALOGD("########## TEARDOWN :: by sapark #######################\n");

        stop();

        // if( mSessionID > 0 ) {
        //     mNetSession->destroySession(mSessionID);
        //     mSessionID = 0;
        // }

        // if(mUibcSessionID>0) {
        //     bTerminatedUibc = true;
        //     mNetSession->destroySession(mUibcSessionID);
        //     mUibcSessionID = 0x00;
        // }
     
        // if (mNotify == NULL) {
        //     looper()->stop();
        // } else {
        //     sp<AMessage> notify = mNotify->dup();
        //     notify->setInt32("what", kWhatDisconnected);
        //     notify->post();
        // }
    }
     
}

void WifiDisplaySink::sendErrorResponse(
        int32_t sessionID,
        const char *errorDetail,
        int32_t cseq) {
    AString response;
    response.append("RTSP/1.0 ");
    response.append(errorDetail);
    response.append("\r\n");

    AppendCommonResponse(&response, cseq);

    response.append("\r\n");

    ALOGD("RESPONSE ===========>");
    ALOGD("%s", response.c_str());
    ALOGD("<===========");
    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

// static
void WifiDisplaySink::AppendCommonResponse(AString *response, int32_t cseq) {
    time_t now = time(NULL);
    struct tm *now2 = gmtime(&now);
    char buf[128];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", now2);

    response->append("Date: ");
    response->append(buf);
    response->append("\r\n");

    response->append(StringPrintf("User-Agent: %s\r\n", sUserAgent.c_str()));

    if (cseq >= 0) {
        response->append(StringPrintf("CSeq: %d\r\n", cseq));
    }
}



void * doUibcLoop(void *data) {

    sp<WifiDisplaySink> sink = (WifiDisplaySink *)data;
    ALOGD("######################## init_getevent");

    int device_id=0;
    init_getevent();

    ALOGD("######################## doUibcLoop1 termitated:%d", sink->bTerminatedUibc);
    while (!sink->bTerminatedUibc){
        int timeout = 250;
        struct input_event2 event;
        memset( &event, 0, sizeof(struct input_event2) );
        int event_result= get_event( (struct input_event*)&event, timeout,&device_id);
        if(event_result==-1){
            // ALOGD("######################## doUibcLoop1 eventresult:noevent");
            continue;
        }
            

        if( event_result == 0x00 )
        {
            //send to server
            //LOGI("INPUT - TIME(%ld-%ld) Type(%04x) Code(%04x) Value(%08x)", event.time.tv_sec, event.time.tv_usec,  event.type, event.code, event.value);

            //ALOGD("######################## get_event() time:%d, id:%d, mouse:%d,", event.time.tv_sec, device_id, mouse_device_id);

            if( event.time.tv_sec != 0x00 )
            {
                if(device_id==touch_device_id)
                {
                    //ALOGD("######################## get_event() type:%d, ABS:%d, SYN:%d", event.type, EV_ABS, EV_SYN);
                    if(event.type == EV_ABS) {
                        doMakeUibcEvent(&event);
                    } else if (event.type == EV_SYN) {
                        doSendUibcEvent(sink, &event);
                    }
                }
            }
        }
    }

    uninit_getevent();

    // ALOGD("######################## uninit_getevent");
    // pthread_exit(NULL);
    ALOGD("######################## uninit_getevent exit");

    return 0x00;
}

int32_t slot = -1;
int32_t trackid = -1;
int32_t posx = -1;
int32_t posy = -1;
int32_t evt_trackid = 0x00;
int32_t evt_posx = -1;
int32_t evt_posy = -1;


void doMakeUibcEvent(struct input_event2 * event) {
    switch(event->code) {
    case ABS_MT_TRACKING_ID:
        ALOGD("##################### doMakeUibcEvent - ABS_MT_TRACKING_ID : %d", event->value);
        if(slot == -1)
            slot = 1;
        if(slot > 1){  // single touch
            return;
        }
        evt_trackid = event->value;
        break;
    case ABS_MT_POSITION_X:
       // ALOGD("##################### doMakeUibcEvent - ABS_MT_POSITION_X : %d", event->value);
        if(slot > 1){  // single touch
            return;
        }
        evt_posx = event->value;
        break;
    case ABS_MT_POSITION_Y:
       // ALOGD("##################### doMakeUibcEvent - ABS_MT_POSITION_Y : %d", event->value);
        if(slot > 1){  // single touch
            return;
        }
        evt_posy = event->value;
        break;
    case ABS_MT_SLOT:
        ALOGD("##################### doMakeUibcEvent - ABS_MT_SLOT : %d", event->value);
        slot = event->value;
        break;
    // lollipop
    case ABS_MT_PRESSURE:
        slot = event->value;
        evt_trackid = event->value;
        break;
    case ABS_MT_TOUCH_MAJOR:
        if (event->value <= 0x00) {
            evt_trackid = 0xffffffff;
        }
        break;
    }
}

int32_t lastevent = 0x00;   // 1:down, 2:up, 3:move

void doSendUibcEvent(sp<WifiDisplaySink> sink, struct input_event2 * event) {
    
    if(event->code != SYN_REPORT) {
       // ALOGD("##################### doSendUibdEvent - isn't define code");        
        return;
    }

    // 2byte: version, timestamp_flag, inputcategory
    // 2byte: length
    // 1byte: input type (0:touch down, 1:touch up, 2: touchmove)
    // 2byte: length
    // 1byte: num of pointers (defautl: 1) (just single)
    // 1byte: pointer ID
    // 2byte: posx
    // 2byte: posy
    unsigned char data[13] = {0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char data2[15] = {0x10, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};

    ALOGD("##################### doSendUibdEvent - evt %d , track %d", evt_trackid, trackid);


    if(evt_trackid == 0xffffffff && trackid != 0xffffffff) {
        // touch up
        ALOGD("##################### doSendUibdEvent - TOUCH UP!!!");
        trackid = evt_trackid;
        data[4] = 1;    // type;
        data2[6] = 1;    // type;
    }
    else if (evt_trackid == 0x00 && trackid > 0x00) {
        // touch move
        ALOGD("##################### doSendUibdEvent - TOUCH MOVE!!!");
        data[4] = 2;    // type;
        data2[6] = 2;    // type;
    }
    else if (evt_trackid > 0x00 && evt_trackid == trackid) {
        ALOGD("##################### doSendUibdEvent - TOUCH MOVE (on lollipop)!!!");
        data[4] = 2;    // type;
        data2[6] = 2;    // type;
    }
    else if (evt_trackid > 0x00) {
        // touch down
        ALOGD("##################### doSendUibdEvent - TOUCH DOWN!!!");
        trackid = evt_trackid;
        data[4] = 0;    // type;
        data2[6] = 0;    // type;
    }
    else {
        ALOGE("########### doSendUibdEvent - ?????? ");
        return;
    }

    if (evt_posx >= 0x00) posx = evt_posx;
    if (evt_posy >= 0x00) posy = evt_posy;

    // int32_t x = (1280 * posx) / 1024;
    // int32_t y = (720 * posy) / 552;

    int w = sink->mWindowSize ? sink->mWidth : 1280;
    int h = sink->mWindowSize ? sink->mHeight : 720;

    int32_t x = (w * posx) / 1024;
    int32_t y = (h * posy) / 552;

    ALOGE("############ doSendUibcEvent - posx:%d, posy:%d, x:%d, y:%d", posx, posy, x, y);

    data[9] = ((x >> 8)&0xFF); 
    data[10] = (x & 0xFF);
    data[11] = ((y >> 8)&0xFF); 
    data[12] = (y & 0xFF);


    data2[11] = ((posx >> 8)&0xFF); 
    data2[12] = (posx & 0xFF);
    data2[13] = ((posy >> 8)&0xFF); 
    data2[14] = (posy & 0xFF);

    if (sink->mNetSession > 0x00 && sink->mUibcSessionID > 0x00){

        // ALOGE("########### doSendUibdEvent - x:0x%X, 0x%X%X, y:%X, 0x%X%X", posx, data[9], data[10], posy, data[11], data[12]);
        sink->mNetSession->sendRequest(sink->mUibcSessionID, data, 13, false, -1ll, true);
        //sink->mNetSession->sendRequest(sink->mUibcSessionID, data2, 15, false, -1ll, true);
    } else {
        ALOGE("########### doSendUibdEvent - uibcsession:%d", sink->mUibcSessionID);
    }

    // init
    evt_trackid = 0x00;
    evt_posx = -1;
    evt_posy = -1;
    slot = -1;
}

}  // namespace android
