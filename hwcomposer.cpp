#include <hwcomposer_drm.h>
#include <ion/ion.h>

struct hwc_context;

static void kms_plane_print (int fd, uint32_t id)
{
    char buffer[1024];
    int i;
    drmModePlanePtr plane = drmModeGetPlane(fd, id);
    if (! plane) {
        ALOGE("Failed to get plane %d: %s\n",id, strerror(errno));
        return;
    }

    ALOGI("\t\t%02d: FB %02d (%4dx%4d), CRTC %02d (%4dx%4d),"
        "Possible CRTCs 0x%02x\n", plane->plane_id, plane->fb_id,
        plane->crtc_x, plane->crtc_y, plane->crtc_id, plane->x,
        plane->y, plane->possible_crtcs);
    for (i = 0; i < (int)plane->count_formats; i++)
        sprintf(buffer + 6 * i, "%c%c%c%c,", plane->formats[i] & 0xFF,
                (plane->formats[i] >> 8) & 0xFF,
                (plane->formats[i] >> 16) & 0xFF,
                (plane->formats[i] >> 24) & 0xFF);
    ALOGI("\t\t Supported Formats:%s\n",buffer);
    drmModeFreePlane(plane);
}

static void drm_list_kms(hwc_context_t *ctx, drmModeResPtr resources,
	    drmModePlaneResPtr planes)
{
    int i;
    ALOGI("KMS Resources:\n");
    ALOGI("\t Dimensions: (%d,%d)->(%d,%d)\n",	resources->min_width, resources->min_height,
	                resources->max_width, resources->max_height);
    ALOGI("\tFBs:\n");
    for (i = 0; i < resources->count_fbs; i++)
        ALOGI("\t\t%d\n", resources->fbs[i]);
    ALOGI("\tPlanes:\n");
    for (i = 0; i < (int)planes->count_planes; i++)
	    kms_plane_print(ctx->drm_fd, planes->planes[i]);
    ALOGI("\tCRTCs:\n");
    for (i = 0; i < resources->count_crtcs; i++)
	    ALOGI("\t\t%d\n", resources->crtcs[i]);
    ALOGI("\tEncoders:\n");
    for (i = 0; i < resources->count_encoders; i++)
        ALOGI("\t\t%d\n", resources->encoders[i]);
    ALOGI("\tConnectors:\n");
    for (i = 0; i < resources->count_connectors; i++)
        ALOGI("\t\t%d\n", resources->connectors[i]);
}
static int send_vsync_request(hwc_context_t *ctx, int disp)
{
    int ret = 0;
    drmVBlank vbl;

    vbl.request.type = (drmVBlankSeqType) (DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT);
    if (disp) //TODO: Use high crtc flag
        vbl.request.type = (drmVBlankSeqType) (DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT | DRM_VBLANK_SECONDARY);
    vbl.request.sequence = 1;
    vbl.request.signal = (unsigned long)&ctx->displays[disp];

    ret = drmWaitVBlank(ctx->drm_fd, &vbl);
    if (ret < 0)
        ALOGE("Failed to request vsync %d", errno);
    return ret;
}

static void vblank_handler(int fd, unsigned int frame, unsigned int sec,
        unsigned int usec, void *data)
{
    kms_display_t *kdisp = (kms_display_t *)data;
    const hwc_procs_t *procs = kdisp->ctx->cb_procs;

    if (kdisp->vsync_on) {
        int64_t ts = sec * (int64_t)1000000000 + usec * (int64_t)1000;
        procs->vsync(procs, 0, ts);
    }
}

static int init_display(hwc_context_t *ctx)
{
	kms_display_t *d = &ctx->displays[HWC_DISPLAY_PRIMARY];
	int drm_fd;
	int i, n;
	drmModeResPtr resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	drmModeModeInfoPtr mode;
	uint32_t possible_crtcs;

	 /* Open DRM device */
	/* TODO: find driver name dynamically */
	drm_fd = drmOpen("sti", NULL);
	if (drm_fd == -1) {
		ALOGE("Failed to open DRM: %s\n", strerror(errno));
		return -EINVAL;
	}

	resources = drmModeGetResources(drm_fd);
	if (!resources) {
		ALOGE("Failed to get resources: %s\n", strerror(errno));
		goto close;
	}

	connector = drmModeGetConnector(drm_fd, resources->connectors[HWC_DISPLAY_PRIMARY]);
	if (!connector) {
		ALOGE("No connector for DISPLAY %d\n", HWC_DISPLAY_PRIMARY);
		goto free_ressources;
	}

	mode = &connector->modes[0];

	encoder = drmModeGetEncoder(drm_fd, connector->encoders[0]);
	if (!encoder) {
		ALOGE("Failed to get encoder\n");
		goto free_connector;
	}

	i = 0;
	n = encoder->possible_crtcs;
	while (!(n & 1)) {
		n >>= 1;
		i++;
	}

	ctx->drm_fd = drm_fd;
	d->con = connector;
	d->enc = encoder;
	d->crtc_id = resources->crtcs[i];
	d->mode = mode;
	d->evctx.version = DRM_EVENT_CONTEXT_VERSION;
	d->evctx.vblank_handler = vblank_handler;
	d->ctx = ctx;

	return 0;

free_connector:
	drmModeFreeConnector(connector);
free_ressources:
	drmModeFreeResources(resources);
close:
	drmClose(drm_fd);
	return -1;
}

static void destroy_display(int drm_fd, kms_display_t *d)
{
    if (d->crtc)
        drmModeFreeCrtc(d->crtc);
    if (d->enc)
        drmModeFreeEncoder(d->enc);
    if (d->con)
        drmModeFreeConnector(d->con);
    memset(d, 0, sizeof(*d));
}

static void dump_layer(hwc_layer_1_t *l)
{
    ALOGI("Layer type=%d, flags=0x%08x, handle=0x%p, tr=0x%02x, blend=0x%04x,"
        " {%d,%d,%d,%d} -> {%d,%d,%d,%d}, acquireFd=%d, releaseFd=%d",
        l->compositionType, l->flags, l->handle, l->transform, l->blending,
        l->sourceCrop.left, l->sourceCrop.top,
        l->sourceCrop.right, l->sourceCrop.bottom,
        l->displayFrame.left, l->displayFrame.top,
        l->displayFrame.right, l->displayFrame.bottom,
        l->acquireFenceFd, l->releaseFenceFd);
}

static void *event_handler (void *arg)
{
    hwc_context_t *ctx = (hwc_context_t *)arg;
    pthread_mutex_t *mutex = &ctx->ctx_mutex;
    int drm_fd = ctx->drm_fd;
    drmEventContext evctx = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .vblank_handler = vblank_handler,
    };
    struct pollfd pfds[1] = { { .fd = drm_fd, .events = POLLIN } };

    while (1) {
        int ret = poll(pfds, ARRAY_SIZE(pfds), 60000);
        if (ret < 0) {
            ALOGE("Event handler error %d", errno);
            break;
        } else if (ret == 0) {
            ALOGI("Event handler timeout");
            continue;
        }
        for (int i=0; i<ret; i++) {
            if (pfds[i].fd == drm_fd)
                drmHandleEvent(drm_fd, &evctx);
        }
    }
    return NULL;
}

static int update_display(hwc_context_t *ctx, int disp,
        hwc_display_contents_1_t *display)
{
    int dmabuf_fd = 0, ret = 0;
    int width = 0, height = 0;
    uint32_t fb = 0;
    kms_display_t *kdisp = &ctx->displays[disp];
    if (!kdisp->con)
        return 0;

    int nLayers = display->numHwLayers;
    hwc_layer_1_t *layers = &display->hwLayers[0];
    hwc_layer_1_t *target = NULL;
    for (int i = 0; i < nLayers; i++) {
        if (layers[i].compositionType == HWC_FRAMEBUFFER_TARGET)
            target = &layers[i];
    }
    if (!target) {
        ALOGE("No target");
        return -EINVAL;
    }

   private_handle_t const *hnd = reinterpret_cast<private_handle_t const *>(target->handle);

   if (!(hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)) {
	AERR("private_handle_t isn't using ION, hnd->flags %d", hnd->flags);
	return 0;
   }

     dmabuf_fd = hnd->share_fd;
    width = hnd->width;
    height = hnd->height;

    uint32_t bo[4] = { 0 };
    ret = drmPrimeFDToHandle(ctx->drm_fd, dmabuf_fd, &bo[0]);
    if (ret) {
        ALOGE("cannot create handle from FD :%d\n", dmabuf_fd);
        return ret;
    }
    uint32_t pitch[4] = { width * 4}; //stride
    uint32_t offset[4] = { 0 };

    ret = drmModeAddFB2(ctx->drm_fd, width, height, DRM_FORMAT_ARGB8888,
            bo, pitch, offset, &fb, 0);
    if (ret) {
        ALOGE("cannot create framebuffer (%d): %m\n",errno);
        return ret;
    }
    ret = drmModeSetCrtc(ctx->drm_fd, kdisp->crtc_id, fb, 0, 0,
                &kdisp->con->connector_id, 1, kdisp->mode);
    if (ret) {
        ALOGE("cannot set CRTC for connector %u (%d): %m\n", kdisp->con->connector_id, ret);
        return ret;
    }
    /* Clean up */
    if (kdisp->last_fb)
        drmModeRmFB(ctx->drm_fd, kdisp->last_fb);
    kdisp->last_fb = fb;

    return 0;

}


static int hwc_prepare (struct hwc_composer_device_1 *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    hwc_display_contents_1_t *contents = displays[HWC_DISPLAY_PRIMARY];

    if (! numDisplays || ! displays)
        return 0;
    if ((contents->flags & HWC_GEOMETRY_CHANGED) == 0)
        return 0;
    
    for (size_t i = 0; i < contents->numHwLayers; i++) {
        hwc_layer_1_t *layers = &contents->hwLayers[i];
        //dump_layer(&layers[i]);
        if (layers[i].compositionType == HWC_FRAMEBUFFER_TARGET)
            continue;
        layers[i].compositionType = HWC_FRAMEBUFFER;
    }
    return 0;
}

static int hwc_set (struct hwc_composer_device_1 *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    hwc_display_contents_1_t *contents = displays[HWC_DISPLAY_PRIMARY];
    int ret = 0;
    hwc_context_t *ctx = to_ctx(dev);
    size_t i = 0;

    if (! numDisplays || ! displays)
        return 0;
    /*for (i = 0; i < contents->numHwLayers; i++) {
        dump_layer(&contents->hwLayers[i]);
    }*/
    for (i=0; i < numDisplays; i++) {
        ret = update_display(ctx, i, displays[i]);
        if (ret < 0)
            break;
    }
    return ret;

}

static int hwc_eventControl (struct hwc_composer_device_1* dev, int disp,
        int event, int enabled)
{
    if (disp < 0 || disp >= HWC_NUM_DISPLAY_TYPES)
        return -EINVAL;    
    hwc_context_t *ctx = to_ctx(dev);

    switch (event) {
        case HWC_EVENT_VSYNC:
            ctx->displays[disp].vsync_on = enabled;
            if (enabled)
                send_vsync_request(ctx, disp);
            return 0;
        default:
           return -EINVAL;
    }
}

static int hwc_query (struct hwc_composer_device_1* dev, int what, int *value)
{
    hwc_context_t *ctx = to_ctx(dev);
    int refreshRate = 60;
    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        value[0] = 1;   //support the background layer
        break;
    case HWC_VSYNC_PERIOD:
        *value = 1000000000 / refreshRate;
        break;
   case HWC_DISPLAY_TYPES_SUPPORTED:
        *value = HWC_DISPLAY_PRIMARY_BIT;
        if (ctx->displays[HWC_DISPLAY_EXTERNAL].con)
            *value |= HWC_DISPLAY_EXTERNAL_BIT;
        break;
    default:
        return -EINVAL; //unsupported query
    }
    return 0;
}

static void hwc_registerProcs (struct hwc_composer_device_1* dev, 
        hwc_procs_t const* procs)
{
    hwc_context_t *ctx = to_ctx(dev);

    ctx->cb_procs = procs;
}

static int hwc_getDisplayConfigs (struct hwc_composer_device_1* dev, 
        int disp, uint32_t *configs, size_t *numConfigs)
{
    if (*numConfigs == 0)
        return 0;
    if (disp == HWC_DISPLAY_PRIMARY) {
        *configs = HWC_DEFAULT_CONFIG;
        *numConfigs = 1;
        return 0;
    }
    return -EINVAL;
}

static int hwc_getDisplayAttributes (struct hwc_composer_device_1* dev, int disp,
        uint32_t config, const uint32_t* attributes, int32_t* values)
{
    hwc_context_t *ctx = to_ctx(dev);
    kms_display_t *d = &ctx->displays[disp];

    if (disp != HWC_DISPLAY_PRIMARY || config != HWC_DEFAULT_CONFIG)
        return -EINVAL;
    while (*attributes != HWC_DISPLAY_NO_ATTRIBUTE) {
        switch (*attributes) {
            case HWC_DISPLAY_VSYNC_PERIOD:
                *values = 1000000000 / 60;
                break;
            case HWC_DISPLAY_WIDTH:
                *values = d->mode->hdisplay;
                break;
            case HWC_DISPLAY_HEIGHT:
                *values = d->mode->vdisplay;
                break;
            case HWC_DISPLAY_DPI_X:
            case HWC_DISPLAY_DPI_Y:
                *values = 240000;
                break;
            default:
                ALOGE("unknown display attribute %u\n", *attributes);
                *values = 0;
                return -EINVAL;
        }
        attributes++;
        values++;
    }
    return 0;
}

static int hwc_blank(struct hwc_composer_device_1* dev, int disp, int blank)
{
    return 0;
}

static void hwc_dump(struct hwc_composer_device_1* dev, char *buff,
                                int buff_len)
{
        ALOGD("%s", __func__);
}

static int hwc_device_close(struct hw_device_t *dev)
{
    hwc_context_t *ctx = to_ctx(dev);

    if (!ctx)
        return 0;

    destroy_display(ctx->drm_fd, &ctx->displays[HWC_DISPLAY_PRIMARY]);
    drmClose(ctx->drm_fd);
    free(ctx);

    return 0;
}

static int hwc_device_open(const struct hw_module_t *module, const char *name,
        struct hw_device_t **device)
{
    hwc_context_t *ctx;
    drmModeResPtr resources;
    drmModePlaneResPtr planes;
    int err = 0;
    int ret = 0;
    int drm_fd = 0;

    if (strcmp(name, HWC_HARDWARE_COMPOSER))
	return -EINVAL;
    ctx = (hwc_context_t *) calloc(1, sizeof(*ctx));

    /* Initialize the procs */
    ctx->device.common.tag = HARDWARE_DEVICE_TAG;
    ctx->device.common.version = HWC_DEVICE_API_VERSION_1_1;
    ctx->device.common.module = (struct hw_module_t *)module;
    ctx->device.common.close = hwc_device_close;

    ctx->device.prepare = hwc_prepare;
    ctx->device.set = hwc_set;
    ctx->device.eventControl = hwc_eventControl;
    ctx->device.blank = hwc_blank;
    ctx->device.query = hwc_query;
    ctx->device.registerProcs = hwc_registerProcs;
    ctx->device.dump = hwc_dump;
    ctx->device.getDisplayConfigs = hwc_getDisplayConfigs;
    ctx->device.getDisplayAttributes = hwc_getDisplayAttributes;

    /* Open Gralloc module */
    ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
			(const struct hw_module_t **)&ctx->gralloc);
    if (ret) {
        ALOGE("Failed to get gralloc module: %s\n",strerror(errno));
        return ret;
    }

    ret = init_display(ctx);
    if (ret) {
        return -EINVAL;
    }

    pthread_attr_t attrs;
    pthread_attr_init(&attrs);
    pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_JOINABLE);
    ret = pthread_create(&ctx->event_thread, &attrs, event_handler, ctx);
    if (ret) {
        ALOGE("Failed to create event thread:%s\n",strerror(ret));
        ret = -ret;
        return ret;
    }

    *device = &ctx->device.common;

    ctx->ion_client = ion_open();
    if (ctx->ion_client < 0)
    {
	   AERR("Could not open ion device for hwcomposer %d", ctx->ion_client);
	   return -EINVAL;
    }
    ALOGE("ion_open success :%d\n", ctx->ion_client);

    return 0;
}

static struct hw_module_methods_t hwc_module_methods = {
    .open = hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "DRM/KMS hwcomposer module",
        .author = "Bibhuti Panigrahi <bibhuti.panigrahi@linaro.org>",
        .methods = &hwc_module_methods,
    }
};
