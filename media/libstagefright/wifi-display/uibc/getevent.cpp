#include "getevent.h"

#ifdef ANDROID
#include <android/log.h>
#define LOG_TAG "N_ECT_SVC_JNI"
#define  LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#define  LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)
#define	 LOGV(...)  __android_log_print(ANDROID_LOG_VERBOSE,LOG_TAG,__VA_ARGS__)
#endif


#ifdef ANDROID

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/limits.h>
#include <sys/poll.h>
#include <errno.h>
//#include <cutils/log.h>



static struct pollfd *ufds;
static char **device_names;
static int nfds;

int keyboard_device_id=-1;
int mouse_device_id=-1;
int xpad_device_id=-1;
int touch_device_id = -1;

static int open_device(const char *device)
{
    int version;
    int fd;
    struct pollfd *new_ufds;
    char **new_device_names;
    char name[80];
    char location[80];
    char idstr[80];
    struct input_id id;

    LOGI("open_device [%s]", device  );
    ///// linux shell command for right
#ifdef EXEC
    FILE * f =NULL;
    f = popen( "insmod /data/local/modules/xpad.ko 2>&1", "r" );
	if ( f == 0 ) {
		LOGI(  "Could not execute 'insmod /data/local/modules/xpad.ko' \n" );
		return 1;
	}
	#define CMD_BUFSIZE 1024
	char out_buf[ CMD_BUFSIZE ];
	while( fgets( out_buf, CMD_BUFSIZE,  f ) ) {
		LOGI("%s", out_buf  );
	}
	pclose( f );
	LOGI("\n\n\n open_device command line execution ended [%s] \n\n\n", device  );
#endif


    fd = open(device, O_RDONLY); // O_RDWR
    if(fd < 0) {
		LOGI("if(fd < 0) fd = open(device, O_RDR) ");
        return -1;
    }
    if(ioctl(fd, EVIOCGVERSION, &version)) {
        LOGI("could not get driver version for %s, %s\n", device, strerror(errno));
        return -1;
    }
    if(ioctl(fd, EVIOCGID, &id)) {
        LOGI("could not get driver id for %s, %s\n", device, strerror(errno));
        return -1;
    }

    name[sizeof(name) - 1] = '\0';
    location[sizeof(location) - 1] = '\0';
    idstr[sizeof(idstr) - 1] = '\0';
    if(ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
        LOGI("could not get device name for %s, %s\n", device, strerror(errno));
        name[0] = '\0';
    }
    if(ioctl(fd, EVIOCGPHYS(sizeof(location) - 1), &location) < 1) {
        LOGI("could not get location for %s, %s\n", device, strerror(errno));
        location[0] = '\0';
    }
    if(ioctl(fd, EVIOCGUNIQ(sizeof(idstr) - 1), &idstr) < 1) {
        LOGI("could not get idstring for %s, %s\n", device, strerror(errno));
        idstr[0] = '\0';
    }

    LOGI( "[%s] - > open_device name [%s] location [%s] EVIOCGUNIQ [%s] \n", device,name,location,idstr);


    int grab_device=0;

    if (strcmp(name, "Microsoft X-Box 360 pad") == 0 || strcmp(name, "Generic X-Box pad") == 0 )
    {
    	LOGI( "[%s] - > open_device name [%s] grab \n", device,name);
    	grab_device	=1;
    }


    if(ioctl(fd, EVIOCGID, &id)==0)
    {
    	LOGI("[%s] open_device vendor:%04x , product:%04x\n",device, id.vendor, id.product);

    	if ( (id.vendor == VENDOR_MICROSOFT && (id.product == PRODUCT_X360 || id.product == PRODUCT_X360W))
    	 		|| (id.vendor == VENDOR_LOGICTECH && (id.product == PRODUCT_F310 || id.product == PRODUCT_F510 || id.product == PRODUCT_F710)))
    	{
    	   	LOGI( "[%s] - > open_device name [%s] grab \n", device,name);
    		grab_device=1;
    	}
    }else
    {
       	LOGI("if(ioctl(fd, EVIOCGID, &id)) failed [%s] \n ",device);
        return -1;
    }

    /// DEVICE TYPE IDENTIFICATION , KBD,MOUSE,XPAD
    {
    	unsigned long evbit[NBITS(EV_MAX)];
    	if (ioctl( fd, EVIOCGBIT(0, sizeof(evbit)), evbit ) != -1)
    	{
            unsigned long absbit[NBITS(ABS_MAX)];
            if (ioctl( fd, EVIOCGBIT(0, sizeof(absbit)), absbit ) != -1){
                LOGI("$$$$$$$$$$$$ dev abs test_bit %X", absbit);
                if (test_bit(EV_ABS, evbit) && test_bit(EV_SYN, evbit) && test_bit(ABS_MT_TRACKING_ID, absbit)) {
                    touch_device_id=nfds;
                    LOGI("\n\n\n [%s] open_device [TTTOUTCH] [%d] nfds \n\n\n",device, nfds);
                }
            }else{
                LOGE("$$$$$$$$$$$$ dev abs test_bit fail");
            }

    		if (test_bit( EV_KEY, evbit )&&test_bit( EV_ABS, evbit )&&!test_bit( EV_REL, evbit ))
    		{
    			if(xpad_device_id==-1)
    			{
    				grab_device=1;
    				xpad_device_id=nfds;
    				LOGI("\n\n\n [%s] open_device [XPAD] [%d] nfds \n\n\n",device, nfds);
    			}else
    				LOGE("\n\n\n [%s] open_device duplicated [XPAD] [%d] nfds -maybe virtual \n\n\n",device, nfds);
    		}else if(test_bit( EV_KEY, evbit )&&!test_bit( EV_ABS, evbit )&&test_bit( EV_REL, evbit ))
    		{
    			//if(mouse_device_id==-1)
    			if(location[0] != '\0')
    			{
    				mouse_device_id=nfds;
    				LOGI("\n\n\n [%s] open_device [MOUSE] [%d] nfds \n\n\n",device, nfds);
    			}else
    			{
    				//mouse_device_id=nfds;
    				LOGE("\n\n\n [%s] open_device duplicated [MOUSE] [%d] nfds -maybe virtual \n\n\n",device, nfds);
    			}
    		}else if(test_bit( EV_KEY, evbit )&&!test_bit( EV_ABS, evbit )&& !test_bit( EV_REL, evbit )
    			  && test_bit(EV_LED, evbit) && test_bit(EV_REP, evbit) )
    		{
    			if(keyboard_device_id==-1)
    			{
    				keyboard_device_id=nfds;
    				LOGI("\n\n\n [%s] open_device [KEYBOARD] maybe [%d] nfds \n\n\n",device, nfds);
    			}else
    				LOGE("\n\n\n [%s] open_device duplicated [KEYBOARD] [%d] nfds -maybe virtual \n\n\n",device, nfds);

    		}
    	}
    }


    if(grab_device)
    {
		if(ioctl(fd, EVIOCGRAB, 1) < 1)
		{
				LOGI( "could not set EVIOCGRAB for %s, %s\n", device, strerror(errno));
		}
    }



    new_ufds = (pollfd*) realloc( ufds, sizeof(ufds[0]) * (nfds + 1));
    if(new_ufds == NULL) {
        LOGI( "out of memory\n");
        return -1;
    }
    ufds = new_ufds;
    new_device_names = (char**)realloc(device_names, sizeof(device_names[0]) * (nfds + 1));
    if(new_device_names == NULL) {
        LOGI( "out of memory\n");
        return -1;
    }
    device_names = new_device_names;
    ufds[nfds].fd = fd;
    ufds[nfds].events = POLLIN;
    device_names[nfds] = strdup(device);

    if(touch_device_id<0x00 && strcmp(name, "gsl")>0x00){
        LOGI("####################### TOUCH id");
        touch_device_id=nfds;
    }else{
        LOGI("####################### no TOUCH %s", name);
    }

    LOGI( "------------ \n open_device function [%d] nfds, [%s] device_names \n ---------------- \n",nfds,device_names[nfds]);

    nfds++;

    return 0;
}

int close_device(const char *device)
{
    int i;
    for(i = 1; i < nfds; i++) {
        if(strcmp(device_names[i], device) == 0) {

        	if( ioctl(ufds[i].fd , EVIOCGRAB, 0) < 1)
        	{
        	   LOGI( "close_device --> could not release EVIOCGRAB for %s, %s\n", device, strerror(errno));
        	}
            int count = nfds - i - 1;
            free(device_names[i]);

            for(int k=0; k<nfds; k++)
            {
            	 LOGI( "close_device -->[%d] nfds [%d] count [%s] devicenames \n", nfds,count,device_names[k]);
            }

            memmove(device_names + i, device_names + i + 1, sizeof(device_names[0]) * count);
            memmove(ufds + i, ufds + i + 1, sizeof(ufds[0]) * count);
            nfds--;

            return 0;
        }
    }
    return -1;
}

static int read_notify(const char *dirname, int nfd)
{
    int res;
    char devname[PATH_MAX];
    char *filename;
    char event_buf[512];
    int event_size;
    int event_pos = 0;
    struct inotify_event *event;

    res = read(nfd, event_buf, sizeof(event_buf));
    if(res < (int)sizeof(*event)) {
        if(errno == EINTR)
            return 0;
        LOGI("could not get event, %s\n", strerror(errno));
        return 1;
    }
    //printf("got %d bytes of event information\n", res);

    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';

    while(res >= (int)sizeof(*event)) {
        event = (struct inotify_event *)(event_buf + event_pos);
        LOGI("read_notify -> %d: %08x \"%s\"\n", event->wd, event->mask, event->len ? event->name : "");
        if(event->len) {
            strcpy(filename, event->name);
            if(event->mask & IN_CREATE) {
                open_device(devname);
            }
            else {
                close_device(devname);
            }
        }
        event_size = sizeof(*event) + event->len;
        res -= event_size;
        event_pos += event_size;
    }
    return 0;
}

static int scan_dir(const char *dirname)
{
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;
    dir = opendir(dirname);
    if(dir == NULL)
        return -1;
    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';
    while((de = readdir(dir))) {
        if(de->d_name[0] == '.' &&
           (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        strcpy(filename, de->d_name);
        open_device(devname);
    }
    closedir(dir);
    return 0;
}

int init_getevent()
{	
    //LOGI("\n\n\nint init_getevent()\n\n\n");

    int res;
    const char *device_path = "/dev/input";

    nfds = 1;
    ufds = (pollfd*)calloc(1, sizeof(ufds[0]));
    ufds[0].fd = inotify_init();
    ufds[0].events = POLLIN;

	res = inotify_add_watch(ufds[0].fd, device_path, IN_DELETE | IN_CREATE);
    if(res < 0) {
        return 1;
    }
	
    //LOGI("\n\n\nint init_getevent() 1 \n\n\n");

    res = scan_dir(device_path);
    if(res < 0) {
        return 1;
    }
	
    //LOGI("\n\n\nint init_getevent() 2 \n\n\n");

    return 0;
}

void uninit_getevent()
{

    int i;

    // by sapark close device
    // for(i = 1; i < nfds; i++) {
    //     // if(strcmp(device_names[i], device) == 0) {

    //         if( ioctl(ufds[i].fd , EVIOCGRAB, 0) < 1)
    //         {
    //            LOGI( "close_device --> could not release EVIOCGRAB for %s, %s\n", device, strerror(errno));
    //         }
    //         int count = nfds - i - 1;
    //         free(device_names[i]);

    //         for(int k=0; k<nfds; k++)
    //         {
    //              LOGI( "close_device -->[%d] nfds [%d] count [%s] devicenames \n", nfds,count,device_names[k]);
    //         }

    //         memmove(device_names + i, device_names + i + 1, sizeof(device_names[0]) * count);
    //         memmove(ufds + i, ufds + i + 1, sizeof(ufds[0]) * count);
    //         nfds--;

    //     //     return 0;
    //     // }
    // }

    for(i = 0; i < nfds; i++) {
        close(ufds[i].fd);
    }
    free(ufds);
    ufds = 0;
    nfds = 0;

	
    //LOGI("\n\n\nint uninit_getevent()\n\n\n");

}

int get_event(struct input_event* event, int timeout, int* device_id)
{
    int res;
    int i;
    int pollres;
    const char *device_path = "/dev/input";

    if(nfds==1)
    {
    	return -1;
    }

    while(1) {
        pollres = poll(ufds, nfds, timeout);
        if (pollres == 0) 
		{
			//LOGI("\n\n\nint get_event() 0 \n\n\n");
            return 1;
        }
        if(ufds[0].revents & POLLIN) {
            read_notify(device_path, ufds[0].fd);

			
			LOGI("\n\n\pollres = poll(ufds, nfds, timeout) ufds[0].revents [%x] [%x] POLLIN \n\n\n",ufds[0].revents,POLLIN);

        }
        for(i = 1; i < nfds; i++) {
            if(ufds[i].revents) {
                if(ufds[i].revents & POLLIN) {
                    res = read(ufds[i].fd, event, sizeof(*event));
                    if(res < (int)sizeof(event)) {
                        LOGI( "could not get event\n");
                        return -1;
                    }
                    *device_id=i;
                    /*
                    switch(i)
                    {
						case MOUSE_DEVICE_EVT :
							*device_id=MOUSE_DEVICE_EVT;
							break;
						case KBD_DEVICE_EVT :
							*device_id=KBD_DEVICE_EVT;
							break;
						case XPAD_DEVICE_EVT :
							*device_id=XPAD_DEVICE_EVT;
							break;
						default:
							LOGI( "\n\n\n\n get_event weird operation [%d] nfds",i);
							*device_id=-1;
							break;
                    }
*/
					//LOGI("\n\n\n nfds [%d] id [%d] \n\n\n",nfds,i);

                    return 0;
                }
            }
        }
    }

    return 0;
}


#elif defined( WIN32 )

int init_getevent()
{
	return -1;
};
void uninit_getevent()
{
	return ;
};
int get_event(struct input_event* event, int timeout)
{
	return -1;
};

#endif
