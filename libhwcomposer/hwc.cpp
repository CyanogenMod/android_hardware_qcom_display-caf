/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <fcntl.h>
#include <errno.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <EGL/egl.h>
#include <utils/Trace.h>
#include <sys/ioctl.h>
#include <overlay.h>
#include <overlayRotator.h>
#include <mdp_version.h>
#include "hwc_utils.h"
#include "hwc_video.h"
#include "hwc_fbupdate.h"
#include "hwc_mdpcomp.h"
#include "external.h"
#include "hwc_copybit.h"

using namespace qhwc;
#define VSYNC_DEBUG 0

static int hwc_device_open(const struct hw_module_t* module,
                           const char* name,
                           struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 2,
        version_minor: 0,
        id: HWC_HARDWARE_MODULE_ID,
        name: "Qualcomm Hardware Composer Module",
        author: "CodeAurora Forum",
        methods: &hwc_module_methods,
        dso: 0,
        reserved: {0},
    }
};

/*
 * Save callback functions registered to HWC
 */
static void hwc_registerProcs(struct hwc_composer_device_1* dev,
                              hwc_procs_t const* procs)
{
    ALOGI("%s", __FUNCTION__);
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    if(!ctx) {
        ALOGE("%s: Invalid context", __FUNCTION__);
        return;
    }
    ctx->proc = procs;

    // Now that we have the functions needed, kick off
    // the uevent & vsync threads
    init_uevent_thread(ctx);
    init_vsync_thread(ctx);
}

//Helper
static void reset(hwc_context_t *ctx, int numDisplays,
                  hwc_display_contents_1_t** displays) {
    memset(ctx->listStats, 0, sizeof(ctx->listStats));
    for(int i = 0; i < MAX_DISPLAYS; i++) {
        hwc_display_contents_1_t *list = displays[i];
        // XXX:SurfaceFlinger no longer guarantees that this
        // value is reset on every prepare. However, for the layer
        // cache we need to reset it.
        // We can probably rethink that later on
        if (LIKELY(list && list->numHwLayers > 1)) {
            for(uint32_t j = 0; j < list->numHwLayers; j++) {
                if(list->hwLayers[j].compositionType != HWC_FRAMEBUFFER_TARGET)
                    list->hwLayers[j].compositionType = HWC_FRAMEBUFFER;
            }
        }

        if(ctx->mFBUpdate[i])
            ctx->mFBUpdate[i]->reset();
        if(ctx->mVidOv[i])
            ctx->mVidOv[i]->reset();
        if(ctx->mCopyBit[i])
            ctx->mCopyBit[i]->reset();
    }
}

//clear prev layer prop flags and realloc for current frame
static void reset_layer_prop(hwc_context_t* ctx, int dpy) {
    int layer_count = ctx->listStats[dpy].numAppLayers;

    if(ctx->layerProp[dpy]) {
       delete[] ctx->layerProp[dpy];
       ctx->layerProp[dpy] = NULL;
    }

    if(layer_count) {
       ctx->layerProp[dpy] = new LayerProp[layer_count];
    }
}

static int display_commit(hwc_context_t *ctx, int dpy) {
    struct mdp_display_commit commit_info;
    memset(&commit_info, 0, sizeof(struct mdp_display_commit));
    commit_info.flags = MDP_DISPLAY_COMMIT_OVERLAY;
    if(ioctl(ctx->dpyAttr[dpy].fd, MSMFB_DISPLAY_COMMIT, &commit_info) == -1) {
       ALOGE("%s: MSMFB_DISPLAY_COMMIT for primary failed", __FUNCTION__);
       return -errno;
    }
    return 0;
}

static int hwc_prepare_primary(hwc_composer_device_1 *dev,
        hwc_display_contents_1_t *list) {
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    const int dpy = HWC_DISPLAY_PRIMARY;
    if (LIKELY(list && list->numHwLayers > 1 &&
        list->numHwLayers <= MAX_NUM_LAYERS) && ctx->dpyAttr[dpy].isActive) {
        uint32_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        if(fbLayer->handle) {
            setListStats(ctx, list, dpy);
            reset_layer_prop(ctx, dpy);
            int ret = ctx->mMDPComp->prepare(ctx, list);
            if(!ret) {
                // IF MDPcomp fails use this route
                ctx->mVidOv[dpy]->prepare(ctx, list);
                ctx->mFBUpdate[dpy]->prepare(ctx, list);
            }
            ctx->mLayerCache[dpy]->updateLayerCache(list);
            // Use Copybit, when MDP comp fails
            if(!ret && ctx->mCopyBit[dpy])
                ctx->mCopyBit[dpy]->prepare(ctx, list, dpy);
        }
    }
    return 0;
}

static int hwc_prepare_external(hwc_composer_device_1 *dev,
        hwc_display_contents_1_t *list, int dpy) {
    hwc_context_t* ctx = (hwc_context_t*)(dev);

    if (LIKELY(list && list->numHwLayers > 1 &&
        list->numHwLayers <= MAX_NUM_LAYERS) &&
        ctx->dpyAttr[dpy].isActive &&
        ctx->dpyAttr[dpy].connected) {
        uint32_t last = list->numHwLayers - 1;
        if(!ctx->dpyAttr[dpy].isPause) {
            hwc_layer_1_t *fbLayer = &list->hwLayers[last];
            if(fbLayer->handle) {
                setListStats(ctx, list, dpy);
                reset_layer_prop(ctx, dpy);
                ctx->mVidOv[dpy]->prepare(ctx, list);
                ctx->mFBUpdate[dpy]->prepare(ctx, list);
                ctx->mLayerCache[dpy]->updateLayerCache(list);
                if(ctx->mCopyBit[dpy])
                    ctx->mCopyBit[dpy]->prepare(ctx, list, dpy);
                ctx->mExtDispConfiguring = false;
            }
        } else {
            // External Display is in Pause state.
            // ToDo:
            // Mark all application layers as OVERLAY so that
            // GPU will not compose. This is done for power
            // optimization
        }
    }
    return 0;
}

static int hwc_prepare(hwc_composer_device_1 *dev, size_t numDisplays,
                       hwc_display_contents_1_t** displays)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    Locker::Autolock _l(ctx->mBlankLock);
    reset(ctx, numDisplays, displays);

    ctx->mOverlay->configBegin();
    ctx->mRotMgr->configBegin();

    for (int32_t i = numDisplays; i >= 0; i--) {
        hwc_display_contents_1_t *list = displays[i];
        switch(i) {
            case HWC_DISPLAY_PRIMARY:
                ret = hwc_prepare_primary(dev, list);
                break;
            case HWC_DISPLAY_EXTERNAL:
            case HWC_DISPLAY_VIRTUAL:
                ret = hwc_prepare_external(dev, list, i);
                break;
            default:
                ret = -EINVAL;
        }
    }

    ctx->mOverlay->configDone();
    ctx->mRotMgr->configDone();

    return ret;
}

static int hwc_eventControl(struct hwc_composer_device_1* dev, int dpy,
                             int event, int enable)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    pthread_mutex_lock(&ctx->vstate.lock);
    switch(event) {
        case HWC_EVENT_VSYNC:
            if (ctx->vstate.enable == enable)
                break;
            ret = hwc_vsync_control(ctx, dpy, enable);
            if(ret == 0) {
                ctx->vstate.enable = !!enable;
                pthread_cond_signal(&ctx->vstate.cond);
            }
            ALOGD_IF (VSYNC_DEBUG, "VSYNC state changed to %s",
                      (enable)?"ENABLED":"DISABLED");
            break;
        default:
            ret = -EINVAL;
    }
    pthread_mutex_unlock(&ctx->vstate.lock);
    return ret;
}

static int hwc_blank(struct hwc_composer_device_1* dev, int dpy, int blank)
{
    ATRACE_CALL();
    hwc_context_t* ctx = (hwc_context_t*)(dev);

    Locker::Autolock _l(ctx->mBlankLock);
    int ret = 0;
    ALOGD("%s: %s display: %d", __FUNCTION__,
          blank==1 ? "Blanking":"Unblanking", dpy);
    switch(dpy) {
        case HWC_DISPLAY_PRIMARY:
            if(blank) {
                ctx->mOverlay->configBegin();
                ctx->mOverlay->configDone();
                ctx->mRotMgr->clear();
                ret = ioctl(ctx->dpyAttr[dpy].fd, FBIOBLANK,FB_BLANK_POWERDOWN);

                if(ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].connected == true) {
                    // Surfaceflinger does not send Blank/unblank event to hwc
                    // for virtual display, handle it explicitly when blank for
                    // primary is invoked, so that any pipes unset get committed
                    if (display_commit(ctx, HWC_DISPLAY_VIRTUAL) < 0) {
                        ret = -1;
                        ALOGE("%s:post failed for virtual display !!",
                                                            __FUNCTION__);
                    } else {
                        ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].isActive = !blank;
                    }
                }
            } else {
                ret = ioctl(ctx->dpyAttr[dpy].fd, FBIOBLANK, FB_BLANK_UNBLANK);
                if(ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].connected == true) {
                    ctx->dpyAttr[HWC_DISPLAY_VIRTUAL].isActive = !blank;
                }
            }
            break;
        case HWC_DISPLAY_EXTERNAL:
            if(blank) {
                // call external framebuffer commit on blank,
                // so that any pipe unsets gets committed
                if (display_commit(ctx, dpy) < 0) {
                    ret = -1;
                    ALOGE("%s:post failed for external display !! ", __FUNCTION__);
                }
            } else {
            }
            break;
        default:
            return -EINVAL;
    }
    // Enable HPD here, as during bootup unblank is called
    // when SF is completely initialized
    ctx->mExtDisplay->setHPD(1);
    if(ret == 0){
        ctx->dpyAttr[dpy].isActive = !blank;
    } else {
        ALOGE("%s: Failed in %s display: %d error:%s", __FUNCTION__,
              blank==1 ? "blanking":"unblanking", dpy, strerror(errno));
        return ret;
    }

    ALOGD("%s: Done %s display: %d", __FUNCTION__,
          blank==1 ? "blanking":"unblanking", dpy);
    return 0;
}

static int hwc_query(struct hwc_composer_device_1* dev,
                     int param, int* value)
{
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    int supported = HWC_DISPLAY_PRIMARY_BIT;

    switch (param) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // Not supported for now
        value[0] = 0;
        break;
    case HWC_DISPLAY_TYPES_SUPPORTED:
        if(ctx->mMDP.hasOverlay)
            supported |= HWC_DISPLAY_EXTERNAL_BIT;
        value[0] = supported;
        break;
    default:
        return -EINVAL;
    }
    return 0;

}


static int hwc_set_primary(hwc_context_t *ctx, hwc_display_contents_1_t* list) {
    ATRACE_CALL();
    int ret = 0;
    const int dpy = HWC_DISPLAY_PRIMARY;
    if (LIKELY(list) && ctx->dpyAttr[dpy].isActive) {
        uint32_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        int fd = -1; //FenceFD from the Copybit(valid in async mode)
        bool copybitDone = false;
        if(ctx->mCopyBit[dpy])
            copybitDone = ctx->mCopyBit[dpy]->draw(ctx, list, dpy, &fd);
        if(list->numHwLayers > 1)
            hwc_sync(ctx, list, dpy, fd);
        if (!ctx->mVidOv[dpy]->draw(ctx, list)) {
            ALOGE("%s: VideoOverlay draw failed", __FUNCTION__);
            ret = -1;
        }
        if (!ctx->mMDPComp->draw(ctx, list)) {
            ALOGE("%s: MDPComp draw failed", __FUNCTION__);
            ret = -1;
        }

        //TODO We dont check for SKIP flag on this layer because we need PAN
        //always. Last layer is always FB
        private_handle_t *hnd = NULL;
        if(copybitDone) {
            hnd = ctx->mCopyBit[dpy]->getCurrentRenderBuffer();
        } else {
            hnd = (private_handle_t *)fbLayer->handle;
        }
        if(fbLayer->compositionType == HWC_FRAMEBUFFER_TARGET && hnd) {
            if(!(fbLayer->flags & HWC_SKIP_LAYER) &&
                (list->numHwLayers > 1)) {
                if (!ctx->mFBUpdate[dpy]->draw(ctx, hnd)) {
                    ALOGE("%s: FBUpdate draw failed", __FUNCTION__);
                    ret = -1;
                }
            }
        }

        if (display_commit(ctx, dpy) < 0) {
            ALOGE("%s: display commit fail!", __FUNCTION__);
            return -1;
        }
    }

    closeAcquireFds(list);
    return ret;
}

static int hwc_set_external(hwc_context_t *ctx,
                            hwc_display_contents_1_t* list, int dpy)
{
    ATRACE_CALL();
    int ret = 0;
    Locker::Autolock _l(ctx->mExtSetLock);

    if (LIKELY(list) && ctx->dpyAttr[dpy].isActive &&
        !ctx->dpyAttr[dpy].isPause &&
        ctx->dpyAttr[dpy].connected) {
        uint32_t last = list->numHwLayers - 1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[last];
        int fd = -1; //FenceFD from the Copybit(valid in async mode)
        bool copybitDone = false;
        if(ctx->mCopyBit[dpy])
            copybitDone = ctx->mCopyBit[dpy]->draw(ctx, list, dpy, &fd);

        if(list->numHwLayers > 1)
            hwc_sync(ctx, list, dpy, fd);

        if (!ctx->mVidOv[dpy]->draw(ctx, list)) {
            ALOGE("%s: VideoOverlay::draw fail!", __FUNCTION__);
            ret = -1;
        }

        private_handle_t *hnd = NULL;
        if(copybitDone) {
            hnd = ctx->mCopyBit[dpy]->getCurrentRenderBuffer();
        } else {
            hnd = (private_handle_t *)fbLayer->handle;
        }

        if(fbLayer->compositionType == HWC_FRAMEBUFFER_TARGET &&
                !(fbLayer->flags & HWC_SKIP_LAYER) && hnd &&
                (list->numHwLayers > 1)) {
            if (!ctx->mFBUpdate[dpy]->draw(ctx, hnd)) {
                ALOGE("%s: FBUpdate::draw fail!", __FUNCTION__);
                ret = -1;
            }
        }

        if (display_commit(ctx, dpy) < 0) {
            ALOGE("%s: display commit fail!", __FUNCTION__);
            return -1;
        }
    }

    closeAcquireFds(list);
    return ret;
}

static int hwc_set(hwc_composer_device_1 *dev,
                   size_t numDisplays,
                   hwc_display_contents_1_t** displays)
{
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    Locker::Autolock _l(ctx->mBlankLock);
    for (uint32_t i = 0; i <= numDisplays; i++) {
        hwc_display_contents_1_t* list = displays[i];
        switch(i) {
            case HWC_DISPLAY_PRIMARY:
                ret = hwc_set_primary(ctx, list);
                break;
            case HWC_DISPLAY_EXTERNAL:
            case HWC_DISPLAY_VIRTUAL:
            /* ToDo: We are using hwc_set_external path for both External and
                     Virtual displays on HWC1.1. Eventually, we will have
                     separate functions when we move to HWC1.2
            */
                ret = hwc_set_external(ctx, list, i);
                break;
            default:
                ret = -EINVAL;
        }
    }
    return ret;
}

int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp,
        uint32_t* configs, size_t* numConfigs) {
    int ret = 0;
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    //in 1.1 there is no way to choose a config, report as config id # 0
    //This config is passed to getDisplayAttributes. Ignore for now.
    switch(disp) {
        case HWC_DISPLAY_PRIMARY:
            if(*numConfigs > 0) {
                configs[0] = 0;
                *numConfigs = 1;
            }
            ret = 0; //NO_ERROR
            break;
        case HWC_DISPLAY_EXTERNAL:
            ret = -1; //Not connected
            if(ctx->dpyAttr[HWC_DISPLAY_EXTERNAL].connected) {
                ret = 0; //NO_ERROR
                if(*numConfigs > 0) {
                    configs[0] = 0;
                    *numConfigs = 1;
                }
            }
            break;
    }
    return ret;
}

int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
        uint32_t config, const uint32_t* attributes, int32_t* values) {

    hwc_context_t* ctx = (hwc_context_t*)(dev);
    //If hotpluggable displays are inactive return error
    if(disp == HWC_DISPLAY_EXTERNAL && !ctx->dpyAttr[disp].connected) {
        return -1;
    }

    //From HWComposer
    static const uint32_t DISPLAY_ATTRIBUTES[] = {
        HWC_DISPLAY_VSYNC_PERIOD,
        HWC_DISPLAY_WIDTH,
        HWC_DISPLAY_HEIGHT,
        HWC_DISPLAY_DPI_X,
        HWC_DISPLAY_DPI_Y,
        HWC_DISPLAY_NO_ATTRIBUTE,
    };

    const int NUM_DISPLAY_ATTRIBUTES = (sizeof(DISPLAY_ATTRIBUTES) /
            sizeof(DISPLAY_ATTRIBUTES)[0]);

    for (size_t i = 0; i < NUM_DISPLAY_ATTRIBUTES - 1; i++) {
        switch (attributes[i]) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            values[i] = ctx->dpyAttr[disp].vsync_period;
            break;
        case HWC_DISPLAY_WIDTH:
            values[i] = ctx->dpyAttr[disp].xres;
            ALOGD("%s disp = %d, width = %d",__FUNCTION__, disp,
                    ctx->dpyAttr[disp].xres);
            break;
        case HWC_DISPLAY_HEIGHT:
            values[i] = ctx->dpyAttr[disp].yres;
            ALOGD("%s disp = %d, height = %d",__FUNCTION__, disp,
                    ctx->dpyAttr[disp].yres);
            break;
        case HWC_DISPLAY_DPI_X:
            values[i] = (int32_t) (ctx->dpyAttr[disp].xdpi*1000.0);
            break;
        case HWC_DISPLAY_DPI_Y:
            values[i] = (int32_t) (ctx->dpyAttr[disp].ydpi*1000.0);
            break;
        default:
            ALOGE("Unknown display attribute %d",
                    attributes[i]);
            return -EINVAL;
        }
    }
    return 0;
}

void hwc_dump(struct hwc_composer_device_1* dev, char *buff, int buff_len)
{
    hwc_context_t* ctx = (hwc_context_t*)(dev);
    android::String8 aBuf("");
    dumpsys_log(aBuf, "Qualcomm HWC state:\n");
    dumpsys_log(aBuf, "  MDPVersion=%d\n", ctx->mMDP.version);
    dumpsys_log(aBuf, "  DisplayPanel=%c\n", ctx->mMDP.panel);
    ctx->mMDPComp->dump(aBuf);
    if (ctx->mCopyBit[HWC_DISPLAY_PRIMARY])
        ctx->mCopyBit[HWC_DISPLAY_PRIMARY]->dump(aBuf);
    char ovDump[2048] = {'\0'};
    ctx->mOverlay->getDump(ovDump, 2048);
    dumpsys_log(aBuf, ovDump);
    ovDump[0] = '\0';
    ctx->mRotMgr->getDump(ovDump, 2048);
    dumpsys_log(aBuf, ovDump);
    strlcpy(buff, aBuf.string(), buff_len);
}

static int hwc_device_close(struct hw_device_t *dev)
{
    if(!dev) {
        ALOGE("%s: NULL device pointer", __FUNCTION__);
        return -1;
    }
    closeContext((hwc_context_t*)dev);
    free(dev);

    return 0;
}

static int hwc_device_open(const struct hw_module_t* module, const char* name,
                           struct hw_device_t** device)
{
    int status = -EINVAL;

    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
        struct hwc_context_t *dev;
        dev = (hwc_context_t*)malloc(sizeof(*dev));
        memset(dev, 0, sizeof(*dev));

        //Initialize hwc context
        initContext(dev);

        //Setup HWC methods
        dev->device.common.tag          = HARDWARE_DEVICE_TAG;
        dev->device.common.version      = HWC_DEVICE_API_VERSION_1_1;
        dev->device.common.module       = const_cast<hw_module_t*>(module);
        dev->device.common.close        = hwc_device_close;
        dev->device.prepare             = hwc_prepare;
        dev->device.set                 = hwc_set;
        dev->device.eventControl        = hwc_eventControl;
        dev->device.blank               = hwc_blank;
        dev->device.query               = hwc_query;
        dev->device.registerProcs       = hwc_registerProcs;
        dev->device.dump                = hwc_dump;
        dev->device.getDisplayConfigs   = hwc_getDisplayConfigs;
        dev->device.getDisplayAttributes = hwc_getDisplayAttributes;
        *device = &dev->device.common;
        status = 0;
    }
    return status;
}
