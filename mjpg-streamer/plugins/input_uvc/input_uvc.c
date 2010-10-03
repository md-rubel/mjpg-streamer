/*******************************************************************************
# Linux-UVC streaming input-plugin for MJPG-streamer                           #
#                                                                              #
# This package work with the Logitech UVC based webcams with the mjpeg feature #
#                                                                              #
# Copyright (C) 2005 2006 Laurent Pinchart &&  Michel Xhaard                   #
#                    2007 Lucas van Staden                                     #
#                    2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; either version 2 of the License, or            #
# (at your option) any later version.                                          #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <linux/videodev.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>

#include "../../utils.h"
//#include "../../mjpg_streamer.h"
#include "v4l2uvc.h" // this header will includes the ../../mjpg_streamer.h
#include "huffman.h"
#include "jpeg_utils.h"
#include "dynctrl.h"

#define INPUT_PLUGIN_NAME "UVC webcam grabber"
#define MAX_ARGUMENTS 32

/*
 * UVC resolutions mentioned at: (at least for some webcams)
 * http://www.quickcamteam.net/hcl/frame-format-matrix/
 */
static const struct {
  const char *string;
  const int width, height;
} resolutions[] = {
  { "QSIF", 160,  120  },
  { "QCIF", 176,  144  },
  { "CGA",  320,  200  },
  { "QVGA", 320,  240  },
  { "CIF",  352,  288  },
  { "VGA",  640,  480  },
  { "SVGA", 800,  600  },
  { "XGA",  1024, 768  },
  { "SXGA", 1280, 1024 }
};

/* private functions and variables to this plugin */
pthread_t cam;
pthread_mutex_t controls_mutex;
struct vdIn *videoIn;
static globals *pglobal;
static int gquality = 80;
static unsigned int minimum_size = 0;
static int dynctrls = 1;

void *cam_thread( void *);
void cam_cleanup(void *);
void help(void);
int input_cmd_new(__u32 control, __s32 value, __u32 typecode);


/*** plugin interface functions ***/
/******************************************************************************
Description.: This function ializes the plugin. It parses the commandline-
              parameter and stores the default and parsed values in the
              appropriate variables.
Input Value.: param contains among others the command-line string
Return Value: 0 if everything is fine
              1 if "--help" was triggered, in this case the calling programm
              should stop running and leave.
******************************************************************************/
int input_init(input_parameter *param) {
  char *argv[MAX_ARGUMENTS]={NULL}, *dev = "/dev/video0", *s;
  int argc=1, width=640, height=480, fps=5, format=V4L2_PIX_FMT_MJPEG, i;

  /* initialize the mutes variable */
  if( pthread_mutex_init(&controls_mutex, NULL) != 0 ) {
    IPRINT("could not initialize mutex variable\n");
    exit(EXIT_FAILURE);
  }

  /* convert the single parameter-string to an array of strings */
  argv[0] = INPUT_PLUGIN_NAME;
  if ( param->parameter_string != NULL && strlen(param->parameter_string) != 0 ) {
    char *arg=NULL, *saveptr=NULL, *token=NULL;

    arg=(char *)strdup(param->parameter_string);

    if ( strchr(arg, ' ') != NULL ) {
      token=strtok_r(arg, " ", &saveptr);
      if ( token != NULL ) {
        argv[argc] = strdup(token);
        argc++;
        while ( (token=strtok_r(NULL, " ", &saveptr)) != NULL ) {
          argv[argc] = strdup(token);
          argc++;
          if (argc >= MAX_ARGUMENTS) {
            IPRINT("ERROR: too many arguments to input plugin\n");
            return 1;
          }
        }
      }
    }
  }

  /* show all parameters for DBG purposes */
  for (i=0; i<argc; i++) {
    DBG("argv[%d]=%s\n", i, argv[i]);
  }

  /* parse the parameters */
  reset_getopt();
  while(1) {
    int option_index = 0, c=0;
    static struct option long_options[] = \
    {
      {"h", no_argument, 0, 0},
      {"help", no_argument, 0, 0},
      {"d", required_argument, 0, 0},
      {"device", required_argument, 0, 0},
      {"r", required_argument, 0, 0},
      {"resolution", required_argument, 0, 0},
      {"f", required_argument, 0, 0},
      {"fps", required_argument, 0, 0},
      {"y", no_argument, 0, 0},
      {"yuv", no_argument, 0, 0},
      {"q", required_argument, 0, 0},
      {"quality", required_argument, 0, 0},
      {"m", required_argument, 0, 0},
      {"minimum_size", required_argument, 0, 0},
      {"n", no_argument, 0, 0},
      {"no_dynctrl", no_argument, 0, 0},
      {"l", required_argument, 0, 0},
      {"led", required_argument, 0, 0},
      {0, 0, 0, 0}
    };

    /* parsing all parameters according to the list above is sufficent */
    c = getopt_long_only(argc, argv, "", long_options, &option_index);

    /* no more options to parse */
    if (c == -1) break;

    /* unrecognized option */
    if (c == '?'){
      help();
      return 1;
    }

    /* dispatch the given options */
    switch (option_index) {
      /* h, help */
      case 0:
      case 1:
        DBG("case 0,1\n");
        help();
        return 1;
        break;

      /* d, device */
      case 2:
      case 3:
        DBG("case 2,3\n");
        dev = strdup(optarg);
        break;

      /* r, resolution */
      case 4:
      case 5:
        DBG("case 4,5\n");
        width = -1;
        height = -1;

        /* try to find the resolution in lookup table "resolutions" */
        for ( i=0; i < LENGTH_OF(resolutions); i++ ) {
          if ( strcmp(resolutions[i].string, optarg) == 0 ) {
            width  = resolutions[i].width;
            height = resolutions[i].height;
          }
        }
        /* done if width and height were set */
        if(width != -1 && height != -1)
          break;
        /* parse value as decimal value */
        width  = strtol(optarg, &s, 10);
        height = strtol(s+1, NULL, 10);
        break;

      /* f, fps */
      case 6:
      case 7:
        DBG("case 6,7\n");
        fps=atoi(optarg);
        break;

      /* y, yuv */
      case 8:
      case 9:
        DBG("case 8,9\n");
        format = V4L2_PIX_FMT_YUYV;
        break;

      /* q, quality */
      case 10:
      case 11:
        DBG("case 10,11\n");
        format = V4L2_PIX_FMT_YUYV;
        gquality = MIN(MAX(atoi(optarg), 0), 100);
        break;

      /* m, minimum_size */
      case 12:
      case 13:
        DBG("case 12,13\n");
        minimum_size = MAX(atoi(optarg), 0);
        break;

      /* n, no_dynctrl */
      case 14:
      case 15:
        DBG("case 14,15\n");
        dynctrls = 0;
        break;

      /* l, led */
      case 16:
      case 17:/*
        DBG("case 16,17\n");
        if ( strcmp("on", optarg) == 0 ) {
          led = IN_CMD_LED_ON;
        } else if ( strcmp("off", optarg) == 0 ) {
          led = IN_CMD_LED_OFF;
        } else if ( strcmp("auto", optarg) == 0 ) {
          led = IN_CMD_LED_AUTO;
        } else if ( strcmp("blink", optarg) == 0 ) {
          led = IN_CMD_LED_BLINK;
        }*/
        break;

      default:
        DBG("default case\n");
        help();
        return 1;
    }
  }

  /* keep a pointer to the global variables */
  pglobal = param->global;

  /* allocate webcam datastructure */
  videoIn = malloc(sizeof(struct vdIn));
  if ( videoIn == NULL ) {
    IPRINT("not enough memory for videoIn\n");
    exit(EXIT_FAILURE);
  }
  memset(videoIn, 0, sizeof(struct vdIn));

  /* display the parsed values */
  IPRINT("Using V4L2 device.: %s\n", dev);
  IPRINT("Desired Resolution: %i x %i\n", width, height);
  IPRINT("Frames Per Second.: %i\n", fps);
  IPRINT("Format............: %s\n", (format==V4L2_PIX_FMT_YUYV)?"YUV":"MJPEG");
  if ( format == V4L2_PIX_FMT_YUYV )
    IPRINT("JPEG Quality......: %d\n", gquality);

  /* open video device and prepare data structure */
  if (init_videoIn(videoIn, dev, width, height, fps, format, 1, pglobal) < 0) {
    IPRINT("init_VideoIn failed\n");
    closelog();
    exit(EXIT_FAILURE);
  }

  /*
   * recent linux-uvc driver (revision > ~#125) requires to use dynctrls
   * for pan/tilt/focus/...
   * dynctrls must get initialized
   */
  if (dynctrls)
    initDynCtrls(videoIn->fd);

  enumerateControls(videoIn, pglobal); // enumerate V4L2 controls after UVC extended mapping
  return 0;
}

/******************************************************************************
Description.: Stops the execution of worker thread
Input Value.: -
Return Value: always 0
******************************************************************************/
int input_stop(void) {
  DBG("will cancel input thread\n");
  pthread_cancel(cam);

  return 0;
}

/******************************************************************************
Description.: spins of a worker thread
Input Value.: -
Return Value: always 0
******************************************************************************/
int input_run(void) {
  pglobal->buf = malloc(videoIn->framesizeIn);
  if (pglobal->buf == NULL) {
    fprintf(stderr, "could not allocate memory\n");
    exit(EXIT_FAILURE);
  }

  pthread_create(&cam, 0, cam_thread, NULL);
  pthread_detach(cam);

  return 0;
}

/*** private functions for this plugin below ***/
/******************************************************************************
Description.: print a help message to stderr
Input Value.: -
Return Value: -
******************************************************************************/
void help(void) {
  int i;

  fprintf(stderr, " ---------------------------------------------------------------\n" \
                  " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
                  " ---------------------------------------------------------------\n" \
                  " The following parameters can be passed to this plugin:\n\n" \
                  " [-d | --device ].......: video device to open (your camera)\n" \
                  " [-r | --resolution ]...: the resolution of the video device,\n" \
                  "                          can be one of the following strings:\n" \
                  "                          ");

  for ( i=0; i < LENGTH_OF(resolutions); i++ ) {
    fprintf(stderr, "%s ", resolutions[i].string);
    if ( (i+1)%6 == 0)
      fprintf(stderr, "\n                          ");
  }
  fprintf(stderr, "\n                          or a custom value like the following" \
                  "\n                          example: 640x480\n");

  fprintf(stderr, " [-f | --fps ]..........: frames per second\n" \
                  " [-y | --yuv ]..........: enable YUYV format and disable MJPEG mode\n" \
                  " [-q | --quality ]......: JPEG compression quality in percent \n" \
                  "                          (activates YUYV format, disables MJPEG)\n" \
                  " [-m | --minimum_size ].: drop frames smaller then this limit, useful\n" \
                  "                          if the webcam produces small-sized garbage frames\n" \
                  "                          may happen under low light conditions\n" \
                  " [-n | --no_dynctrl ]...: do not initalize dynctrls of Linux-UVC driver\n" \
                  " [-l | --led ]..........: switch the LED \"on\", \"off\", let it \"blink\" or leave\n" \
                  "                          it up to the driver using the value \"auto\"\n" \
                  " ---------------------------------------------------------------\n\n");
}

/******************************************************************************
Description.: this thread worker grabs a frame and copies it to the global buffer
Input Value.: unused
Return Value: unused, always NULL
******************************************************************************/
void *cam_thread( void *arg ) {
  /* set cleanup handler to cleanup allocated ressources */
  pthread_cleanup_push(cam_cleanup, NULL);

  while( !pglobal->stop ) {

    while (videoIn->streamingState == STREAMING_PAUSED) {
        usleep(1); // maybe not the best and FIXME
    }

    /* grab a frame */
    if( uvcGrab(videoIn) < 0 ) {
      IPRINT("Error grabbing frames\n");
      exit(EXIT_FAILURE);
    }

    DBG("received frame of size: %d\n", videoIn->buf.bytesused);

    /*
     * Workaround for broken, corrupted frames:
     * Under low light conditions corrupted frames may get captured.
     * The good thing is such frames are quite small compared to the regular pictures.
     * For example a VGA (640x480) webcam picture is normally >= 8kByte large,
     * corrupted frames are smaller.
     */
    if ( videoIn->buf.bytesused < minimum_size ) {
      DBG("dropping too small frame, assuming it as broken\n");
      continue;
    }

    /* copy JPG picture to global buffer */
    pthread_mutex_lock( &pglobal->db );

    /*
     * If capturing in YUV mode convert to JPEG now.
     * This compression requires many CPU cycles, so try to avoid YUV format.
     * Getting JPEGs straight from the webcam, is one of the major advantages of
     * Linux-UVC compatible devices.
     */
    if (videoIn->formatIn == V4L2_PIX_FMT_YUYV) {
      DBG("compressing frame\n");
      pglobal->size = compress_yuyv_to_jpeg(videoIn, pglobal->buf, videoIn->framesizeIn, gquality);
    }
    else {
      DBG("copying frame\n");
      pglobal->size = memcpy_picture(pglobal->buf, videoIn->tmpbuffer, videoIn->buf.bytesused);
    }

#if 0
    /* motion detection can be done just by comparing the picture size, but it is not very accurate!! */
    if ( (prev_size - global->size)*(prev_size - global->size) > 4*1024*1024 ) {
        DBG("motion detected (delta: %d kB)\n", (prev_size - global->size) / 1024);
    }
    prev_size = global->size;
#endif

	/* copy this frame's timestamp to user space */
	pglobal->timestamp = videoIn->buf.timestamp;

	/* signal fresh_frame */
    pthread_cond_broadcast(&pglobal->db_update);
    pthread_mutex_unlock( &pglobal->db );


    /* only use usleep if the fps is below 5, otherwise the overhead is too long */
    if ( videoIn->fps < 5 ) {
        DBG("waiting for next frame for %d us\n", 1000*1000/videoIn->fps);
      usleep(1000*1000/videoIn->fps);
    } else {
        DBG("waiting for next frame\n");
    }
  }

  DBG("leaving input thread, calling cleanup function now\n");
  pthread_cleanup_pop(1);

  return NULL;
}

/******************************************************************************
Description.:
Input Value.:
Return Value:
******************************************************************************/
void cam_cleanup(void *arg) {
  static unsigned char first_run=1;

  if ( !first_run ) {
    DBG("already cleaned up ressources\n");
    return;
  }

  first_run = 0;
  IPRINT("cleaning up ressources allocated by input thread\n");

  close_v4l2(videoIn);
  if (videoIn->tmpbuffer != NULL) free(videoIn->tmpbuffer);
  if (videoIn != NULL) free(videoIn);
  if (pglobal->buf != NULL) free(pglobal->buf);
}

/******************************************************************************
Description.: process commands, allows to set v4l2 controls
Input Value.: * control specifies the selected v4l2 control's id
                see struct v4l2_queryctr in the videodev2.h
              * value is used for control that make use of a parameter.
Return Value: depends in the command, for most cases 0 means no errors and
              -1 signals an error. This is just rule of thumb, not more!
******************************************************************************/
int input_cmd_new(__u32 control, __s32 value, __u32 typecode)
{
    int ret = -1;
    int i = 0;
    DBG("Requested cmd: %d, type: %d value: %d\n", control, typecode, value);
    switch (typecode) {
        case IN_CMD_V4L2: {
            for (i = 0; i<pglobal->in.parametercount; i++) {
                if (pglobal->in.in_parameters[i].ctrl.id == control) {
                    DBG("Found the requested control: %s\n", pglobal->in.in_parameters[i].ctrl.name);
                    ret = v4l2SetControl(videoIn, control, value);
                    if (ret == 0) {
                        pglobal->in.in_parameters[i].value = value;
                    }
                    return ret;
                    break;
                }
            }
        } break;
        case IN_CMD_RESOLUTION: {
            // the value points to the current formats nth resolution
            if (value > (pglobal->in.in_formats[pglobal->in.currentFormat].resolutionCount -1)) {
                DBG("The value is out of range");
                return -1;
            }
            int height = pglobal->in.in_formats[pglobal->in.currentFormat].supportedResolutions[value].height;
            int width = pglobal->in.in_formats[pglobal->in.currentFormat].supportedResolutions[value].width;
            ret = setResolution(videoIn, width, height);
            if (ret == 0) {
                pglobal->in.in_formats[pglobal->in.currentFormat].currentResolution = value;
            }
            return ret;
        } break;
    }
    return ret;
}

