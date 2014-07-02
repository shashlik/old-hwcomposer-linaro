#ifndef ANDROID_HWC_H_
#define ANDROID_HWC_H_
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <cutils/compiler.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>

#include <EGL/egl.h>
#include <sync/sync.h>

#include "gralloc_priv.h"
#include "drm_fourcc.h"
#include "xf86drm.h"
#include "xf86drmMode.h"

#define HWC_DEFAULT_CONFIG 1
#define to_ctx(dev) ((hwc_context_t *)dev)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof((arr)[0]))
#endif

typedef struct kms_display {
    drmModeConnectorPtr con;
    drmModeEncoderPtr enc;
    drmModeCrtcPtr crtc;
    int crtc_id;
    drmModeModeInfoPtr mode;
    drmEventContext evctx;
    uint32_t last_fb;
    int vsync_on;
    struct hwc_context *ctx;
} kms_display_t;

typedef struct hwc_context {
    hwc_composer_device_1_t device;
    pthread_mutex_t ctx_mutex;
    const hwc_procs_t *cb_procs;

    const struct gralloc_module_t *gralloc;

    int drm_fd;
    kms_display_t displays[HWC_NUM_DISPLAY_TYPES];
    int ion_client;

    pthread_t event_thread;

    /* CHECK : bibhuti*/
    int32_t xres;
    int32_t yres;
    int32_t xdpi;
    int32_t ydpi;
    int32_t vsync_period;
} hwc_context_t;

#endif //#ifndef ANDROID_HWC_H_
