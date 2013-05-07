/*
 * Copyright (C) 2012-2013, The Linux Foundation. All rights reserved.
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

#include "hwc_mdpcomp.h"
#include <sys/ioctl.h>
#include "external.h"
#include "qdMetaData.h"
#include "mdp_version.h"
#include <overlayRotator.h>

using overlay::Rotator;
using namespace overlay::utils;
namespace ovutils = overlay::utils;

namespace qhwc {

//==============MDPComp========================================================

IdleInvalidator *MDPComp::idleInvalidator = NULL;
bool MDPComp::sIdleFallBack = false;
bool MDPComp::sDebugLogs = false;
bool MDPComp::sEnabled = false;
int MDPComp::sMaxPipesPerMixer = MAX_PIPES_PER_MIXER;

MDPComp* MDPComp::getObject(const int& width, int dpy) {
    if(width <= MAX_DISPLAY_DIM) {
        return new MDPCompLowRes(dpy);
    } else {
        return new MDPCompHighRes(dpy);
    }
}

MDPComp::MDPComp(int dpy):mDpy(dpy){};

void MDPComp::dump(android::String8& buf)
{
    dumpsys_log(buf,"HWC Map for Dpy: %s \n",
                mDpy ? "\"EXTERNAL\"" : "\"PRIMARY\"");
    dumpsys_log(buf,"PREV_FRAME: layerCount:%2d    mdpCount:%2d \
                cacheCount:%2d \n", mCachedFrame.layerCount,
                mCachedFrame.mdpCount, mCachedFrame.cacheCount);
    dumpsys_log(buf,"CURR_FRAME: layerCount:%2d    mdpCount:%2d \
                fbCount:%2d \n", mCurrentFrame.layerCount,
                mCurrentFrame.mdpCount, mCurrentFrame.fbCount);
    dumpsys_log(buf,"needsFBRedraw:%3s  pipesUsed:%2d  MaxPipesPerMixer: %d \n",
                (mCurrentFrame.needsRedraw? "YES" : "NO"),
                mCurrentFrame.mdpCount, sMaxPipesPerMixer);
    dumpsys_log(buf," ---------------------------------------------  \n");
    dumpsys_log(buf," listIdx | cached? | mdpIndex | comptype  |  Z  \n");
    dumpsys_log(buf," ---------------------------------------------  \n");
    for(int index = 0; index < mCurrentFrame.layerCount; index++ )
        dumpsys_log(buf," %7d | %7s | %8d | %9s | %2d \n",
                    index,
                    (mCurrentFrame.isFBComposed[index] ? "YES" : "NO"),
                    mCurrentFrame.layerToMDP[index],
                    (mCurrentFrame.isFBComposed[index] ?
                     (mCurrentFrame.needsRedraw ? "GLES" : "CACHE") : "MDP"),
                    (mCurrentFrame.isFBComposed[index] ? mCurrentFrame.fbZ :
    mCurrentFrame.mdpToLayer[mCurrentFrame.layerToMDP[index]].pipeInfo->zOrder));
    dumpsys_log(buf,"\n");
}

bool MDPComp::init(hwc_context_t *ctx) {

    if(!ctx) {
        ALOGE("%s: Invalid hwc context!!",__FUNCTION__);
        return false;
    }

    char property[PROPERTY_VALUE_MAX];

    sEnabled = false;
    if((property_get("persist.hwc.mdpcomp.enable", property, NULL) > 0) &&
       (!strncmp(property, "1", PROPERTY_VALUE_MAX ) ||
        (!strncasecmp(property,"true", PROPERTY_VALUE_MAX )))) {
        sEnabled = true;
    }

    sDebugLogs = false;
    if(property_get("debug.mdpcomp.logs", property, NULL) > 0) {
        if(atoi(property) != 0)
            sDebugLogs = true;
    }

    sMaxPipesPerMixer = MAX_PIPES_PER_MIXER;
    if(property_get("debug.mdpcomp.maxpermixer", property, NULL) > 0) {
        if(atoi(property) != 0)
            sMaxPipesPerMixer = true;
    }

    unsigned long idle_timeout = DEFAULT_IDLE_TIME;
    if(property_get("debug.mdpcomp.idletime", property, NULL) > 0) {
        if(atoi(property) != 0)
            idle_timeout = atoi(property);
    }

    //create Idle Invalidator
    idleInvalidator = IdleInvalidator::getInstance();

    if(idleInvalidator == NULL) {
        ALOGE("%s: failed to instantiate idleInvalidator object", __FUNCTION__);
    } else {
        idleInvalidator->init(timeout_handler, ctx, idle_timeout);
    }
    return true;
}

void MDPComp::timeout_handler(void *udata) {
    struct hwc_context_t* ctx = (struct hwc_context_t*)(udata);

    if(!ctx) {
        ALOGE("%s: received empty data in timer callback", __FUNCTION__);
        return;
    }

    if(!ctx->proc) {
        ALOGE("%s: HWC proc not registered", __FUNCTION__);
        return;
    }
    sIdleFallBack = true;
    /* Trigger SF to redraw the current frame */
    ctx->proc->invalidate(ctx->proc);
}

void MDPComp::setMDPCompLayerFlags(hwc_context_t *ctx,
                                   hwc_display_contents_1_t* list) {
    LayerProp *layerProp = ctx->layerProp[mDpy];

    for(int index = 0; index < ctx->listStats[mDpy].numAppLayers; index++) {
        hwc_layer_1_t* layer = &(list->hwLayers[index]);
        if(!mCurrentFrame.isFBComposed[index]) {
            layerProp[index].mFlags |= HWC_MDPCOMP;
            layer->compositionType = HWC_OVERLAY;
            layer->hints |= HWC_HINT_CLEAR_FB;
            mCachedFrame.hnd[index] = NULL;
        } else {
            if(!mCurrentFrame.needsRedraw)
                layer->compositionType = HWC_OVERLAY;
        }
    }
    mCachedFrame.mdpCount = mCurrentFrame.mdpCount;
    mCachedFrame.cacheCount = mCurrentFrame.fbCount;
    mCachedFrame.layerCount = ctx->listStats[mDpy].numAppLayers;
}

MDPComp::FrameInfo::FrameInfo() {
    layerCount = 0;
    reset();
}

void MDPComp::FrameInfo::reset() {

    for(int i = 0 ; i < MAX_PIPES_PER_MIXER && layerCount; i++ ) {
        if(mdpToLayer[i].pipeInfo) {
            delete mdpToLayer[i].pipeInfo;
            mdpToLayer[i].pipeInfo = NULL;
            //We dont own the rotator
            mdpToLayer[i].rot = NULL;
        }
    }

    memset(&mdpToLayer, 0, sizeof(mdpToLayer));
    memset(&layerToMDP, -1, sizeof(layerToMDP));
    memset(&isFBComposed, 0, sizeof(isFBComposed));

    layerCount = 0;
    mdpCount = 0;
    fbCount = 0;
    needsRedraw = false;
    fbZ = 0;
}

MDPComp::LayerCache::LayerCache() {
    reset();
}

void MDPComp::LayerCache::reset() {
    memset(&hnd, 0, sizeof(buffer_handle_t));
    mdpCount = 0;
    cacheCount = 0;
    layerCount = 0;
}

bool MDPComp::isValidDimension(hwc_context_t *ctx, hwc_layer_1_t *layer) {

    private_handle_t *hnd = (private_handle_t *)layer->handle;

    if(!hnd) {
        ALOGE("%s: layer handle is NULL", __FUNCTION__);
        return false;
    }

    int hw_w = ctx->dpyAttr[mDpy].xres;
    int hw_h = ctx->dpyAttr[mDpy].yres;

    hwc_rect_t sourceCrop = layer->sourceCrop;
    hwc_rect_t displayFrame = layer->displayFrame;

    hwc_rect_t crop =  sourceCrop;
    int crop_w = crop.right - crop.left;
    int crop_h = crop.bottom - crop.top;

    hwc_rect_t dst = displayFrame;
    int dst_w = dst.right - dst.left;
    int dst_h = dst.bottom - dst.top;

    if(dst.left < 0 || dst.top < 0 || dst.right > hw_w || dst.bottom > hw_h) {
        hwc_rect_t scissor = {0, 0, hw_w, hw_h };
        qhwc::calculate_crop_rects(crop, dst, scissor, layer->transform);
        crop_w = crop.right - crop.left;
        crop_h = crop.bottom - crop.top;
    }

    /* Workaround for MDP HW limitation in DSI command mode panels where
     * FPS will not go beyond 30 if buffers on RGB pipes are of width or height
     * less than 5 pixels
     * */

    if((crop_w < 5)||(crop_h < 5))
        return false;

    // There is a HW limilation in MDP, minmum block size is 2x2
    // Fallback to GPU if height is less than 2.
    if(crop_h < 2)
        return false;

    return true;
}

ovutils::eDest MDPComp::getMdpPipe(hwc_context_t *ctx, ePipeType type) {
    overlay::Overlay& ov = *ctx->mOverlay;
    ovutils::eDest mdp_pipe = ovutils::OV_INVALID;

    switch(type) {
    case MDPCOMP_OV_DMA:
        mdp_pipe = ov.nextPipe(ovutils::OV_MDP_PIPE_DMA, mDpy);
        if(mdp_pipe != ovutils::OV_INVALID) {
            ctx->mDMAInUse = true;
            return mdp_pipe;
        }
    case MDPCOMP_OV_ANY:
    case MDPCOMP_OV_RGB:
        mdp_pipe = ov.nextPipe(ovutils::OV_MDP_PIPE_RGB, mDpy);
        if(mdp_pipe != ovutils::OV_INVALID) {
            return mdp_pipe;
        }

        if(type == MDPCOMP_OV_RGB) {
            //Requested only for RGB pipe
            break;
        }
    case  MDPCOMP_OV_VG:
        return ov.nextPipe(ovutils::OV_MDP_PIPE_VG, mDpy);
    default:
        ALOGE("%s: Invalid pipe type",__FUNCTION__);
        return ovutils::OV_INVALID;
    };
    return ovutils::OV_INVALID;
}

bool MDPComp::isFrameDoable(hwc_context_t *ctx) {
    int numAppLayers = ctx->listStats[mDpy].numAppLayers;

    if(!isEnabled()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp. not enabled.", __FUNCTION__);
        return false;
    }

    if(ctx->mExtDispConfiguring) {
        ALOGD_IF( isDebug(),"%s: External Display connection is pending",
                  __FUNCTION__);
        return false;
    }

    if(ctx->listStats[mDpy].needsAlphaScale
       && ctx->mMDP.version < qdutils::MDSS_V5) {
        ALOGD_IF(isDebug(), "%s: frame needs alpha downscaling",__FUNCTION__);
        return false;
    }

    return true;
}

/* Checks for conditions where all the layers marked for MDP comp cannot be
 * bypassed. On such conditions we try to bypass atleast YUV layers */
bool MDPComp::isFullFrameDoable(hwc_context_t *ctx,
                                hwc_display_contents_1_t* list){

    int numAppLayers = ctx->listStats[mDpy].numAppLayers;
    int mdpCount = mCurrentFrame.layerCount - mCurrentFrame.fbCount;
    int fbNeeded = int(mCurrentFrame.fbCount != 0);

    if(mDpy > HWC_DISPLAY_PRIMARY){
        ALOGD_IF(isDebug(), "%s: Cannot support External display(s)",
                 __FUNCTION__);
        return false;
    }

    if(mdpCount > (sMaxPipesPerMixer-fbNeeded)) {
        ALOGD_IF(isDebug(), "%s: Exceeds MAX_PIPES_PER_MIXER",__FUNCTION__);
        return false;
    }

    if(pipesNeeded(ctx, list) > getAvailablePipes(ctx)) {
        ALOGD_IF(isDebug(), "%s: Insufficient MDP pipes",__FUNCTION__);
        return false;
    }

    if(isSkipPresent(ctx, mDpy)) {
        ALOGD_IF(isDebug(), "%s: Skip layers present",__FUNCTION__);
        return false;
    }

    //FB composition on idle timeout
    if(sIdleFallBack) {
        sIdleFallBack = false;
        ALOGD_IF(isDebug(), "%s: idle fallback",__FUNCTION__);
        return false;
    }

    //MDP composition is not efficient if layer needs rotator.
    for(int i = 0; i < numAppLayers; ++i) {
        // As MDP h/w supports flip operation, use MDP comp only for
        // 180 transforms. Fail for any transform involving 90 (90, 270).
        hwc_layer_1_t* layer = &list->hwLayers[i];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        if(layer->transform & HWC_TRANSFORM_ROT_90 && !isYuvBuffer(hnd)) {
            ALOGD_IF(isDebug(), "%s: orientation involved",__FUNCTION__);
            return false;
        }

        if(!isYuvBuffer(hnd) && !isValidDimension(ctx,layer)) {
            ALOGD_IF(isDebug(), "%s: Buffer is of invalid width",__FUNCTION__);
            return false;
        }
    }
    return true;
}

/* Checks for conditions where YUV layers cannot be bypassed */
bool MDPComp::isYUVDoable(hwc_context_t* ctx, hwc_layer_1_t* layer) {

    if(isSkipLayer(layer)) {
        ALOGE("%s: Unable to bypass skipped YUV", __FUNCTION__);
        return false;
    }

    if(ctx->mNeedsRotator && ctx->mDMAInUse) {
        ALOGE("%s: No DMA for Rotator",__FUNCTION__);
        return false;
    }

    if(isSecuring(ctx, layer)) {
        ALOGD_IF(isDebug(), "%s: MDP securing is active", __FUNCTION__);
        return false;
    }

    /* Workaround for downscales larger than 4x. Will be removed once decimator
     * block is enabled for MDSS*/
    if(ctx->mMDP.version == qdutils::MDSS_V5) {
        hwc_rect_t crop = layer->sourceCrop;
        hwc_rect_t dst = layer->displayFrame;

        int cWidth = crop.right - crop.left;
        int cHeight = crop.bottom - crop.top;
        int dWidth = dst.right - dst.left;
        int dHeight = dst.bottom - dst.top;

        if((cWidth/dWidth) > 4 || (cHeight/dHeight) > 4)
            return false;
    }
    return true;
}

void  MDPComp::batchLayers() {
    /* Idea is to keep as many contiguous non-updating(cached) layers in FB and
     * send rest of them through MDP. NEVER mark an updating layer for caching.
     * But cached ones can be marked for MDP*/

    int maxBatchStart = -1;
    int maxBatchCount = 0;

    /* All or Nothing is cached. No batching needed */
    if(!mCurrentFrame.fbCount ||
       (mCurrentFrame.fbCount ==  mCurrentFrame.layerCount))
        return;

    /* Search for max number of contiguous (cached) layers */
    int i = 0;
    while (i < mCurrentFrame.layerCount) {
        int count = 0;
        while(mCurrentFrame.isFBComposed[i] && i < mCurrentFrame.layerCount) {
            count++; i++;
        }
        if(count > maxBatchCount) {
            maxBatchCount = count;
            maxBatchStart = i - count;
        }
        if(i < mCurrentFrame.layerCount) i++;
    }

    /* reset rest of the layers for MDP comp */
    for(int i = 0; i < mCurrentFrame.layerCount; i++) {
        if(i != maxBatchStart){
            mCurrentFrame.isFBComposed[i] = false;
        } else {
            i += maxBatchCount;
        }
    }

    mCurrentFrame.fbCount = maxBatchCount;

    ALOGD_IF(isDebug(),"%s: cached count: %d",__FUNCTION__,
             mCurrentFrame.fbCount);
}
void MDPComp::updateLayerCache(hwc_context_t* ctx,
                               hwc_display_contents_1_t* list) {

    int numAppLayers = ctx->listStats[mDpy].numAppLayers;
    int numCacheableLayers = 0;

    if((list->flags & HWC_GEOMETRY_CHANGED) || (isSkipPresent(ctx, mDpy))) {
        ALOGD_IF(isDebug(),"%s: No Caching: \
                 GEOMETRY change: %d SKIP present: %d", __FUNCTION__,
                 (list->flags & HWC_GEOMETRY_CHANGED),isSkipPresent(ctx, mDpy));
        mCachedFrame.reset();
        return;
    }

    for(int i = 0; i < numAppLayers; i++) {
        if (mCachedFrame.hnd[i] == list->hwLayers[i].handle) {
            numCacheableLayers++;
            mCurrentFrame.isFBComposed[i] = true;
        } else {
            mCachedFrame.hnd[i] = list->hwLayers[i].handle;
        }
    }
    mCurrentFrame.fbCount = numCacheableLayers;
    ALOGD_IF(isDebug(),"%s: cached count: %d",__FUNCTION__, numCacheableLayers);
}

int MDPComp::getAvailablePipes(hwc_context_t* ctx) {
    int numDMAPipes = qdutils::MDPVersion::getInstance().getDMAPipes();
    overlay::Overlay& ov = *ctx->mOverlay;

    int numAvailable = ov.availablePipes(mDpy);

    //Reserve DMA for rotator
    if(ctx->mNeedsRotator)
        numAvailable -= numDMAPipes;

    //Reserve pipe(s)for FB
    if(mCurrentFrame.fbCount)
        numAvailable -= pipesForFB();

    return numAvailable;
}

void MDPComp::resetFrameForFB(hwc_context_t* ctx,
                              hwc_display_contents_1_t* list) {
    mCurrentFrame.fbCount = mCurrentFrame.layerCount;
    memset(&mCurrentFrame.isFBComposed, 1,
           sizeof(mCurrentFrame.isFBComposed));
    mCurrentFrame.needsRedraw = true;
}

void MDPComp::updateYUV(hwc_context_t* ctx, hwc_display_contents_1_t* list) {

    int nYuvCount = ctx->listStats[mDpy].yuvCount;
    for(int index = 0;index < nYuvCount; index++){
        int nYuvIndex = ctx->listStats[mDpy].yuvIndices[index];
        hwc_layer_1_t* layer = &list->hwLayers[nYuvIndex];

        if(!isYUVDoable(ctx, layer)) {
            if(!mCurrentFrame.isFBComposed[nYuvIndex]) {
                mCurrentFrame.isFBComposed[nYuvIndex] = true;
                mCurrentFrame.fbCount++;
            }
        } else {
            if(mCurrentFrame.isFBComposed[nYuvIndex]) {
                mCurrentFrame.isFBComposed[nYuvIndex] = false;
                mCurrentFrame.fbCount--;
            }
        }
    }
    ALOGD_IF(isDebug(),"%s: cached count: %d",__FUNCTION__,
             mCurrentFrame.fbCount);
}

int MDPComp::programMDP(hwc_context_t *ctx, hwc_display_contents_1_t* list) {
    int fbZOrder = -1;
    ctx->mDMAInUse = false;

    if(!allocLayerPipes(ctx, list)) {
        ALOGD_IF(isDebug(), "%s: Unable to allocate MDP pipes", __FUNCTION__);
        goto fn_exit;
    }

    for (int index = 0, mdpNextZOrder = 0; index < mCurrentFrame.layerCount;
                                                                      index++) {
        if(!mCurrentFrame.isFBComposed[index]) {

            int mdpIndex = mCurrentFrame.layerToMDP[index];
            hwc_layer_1_t* layer = &list->hwLayers[index];

            MdpPipeInfo* cur_pipe = mCurrentFrame.mdpToLayer[mdpIndex].pipeInfo;
            cur_pipe->zOrder = mdpNextZOrder++;

            if(configure(ctx, layer, mCurrentFrame.mdpToLayer[mdpIndex]) != 0 ){
                ALOGD_IF(isDebug(), "%s: Failed to configure overlay for \
                         layer %d",__FUNCTION__, index);
                goto fn_exit;
            }
        } else if(fbZOrder < 0) {
            fbZOrder = mdpNextZOrder++;
        };
    }

    return fbZOrder;

 fn_exit:
        //Complete fallback to FB
        resetFrameForFB(ctx, list);
        return 0;
}

int MDPComp::prepare(hwc_context_t *ctx, hwc_display_contents_1_t* list) {

    //reset old data
    mCurrentFrame.reset();

    if(!isFrameDoable(ctx)) {
        ALOGD_IF( isDebug(),"%s: MDP Comp not possible for this frame",
                  __FUNCTION__);
        return 0;
    }

    mCurrentFrame.layerCount = ctx->listStats[mDpy].numAppLayers;

    //Iterate layer list for cached layers
    updateLayerCache(ctx, list);

    //Add YUV layers to cached list
    updateYUV(ctx, list);

    //Optimze for bypass
    batchLayers();

    //list is already parsed / batched for optimal mixed mode composition.
    //Check whether layers marked for MDP Composition is actually doable.
    if(!isFullFrameDoable(ctx, list)){
        //All layers marked for MDP comp cannot be bypassed.
        //Try to compose atleast YUV layers through MDP comp and let
        //all the RGB layers compose in FB
        resetFrameForFB(ctx, list);
        updateYUV(ctx, list);
    }

    mCurrentFrame.mdpCount = mCurrentFrame.layerCount -
                             mCurrentFrame.fbCount;

    if(mCurrentFrame.mdpCount) {
        // populate layer and MDP maps
        for(int idx = 0, mdpIdx = 0; idx < mCurrentFrame.layerCount; idx++) {
            if(!mCurrentFrame.isFBComposed[idx]) {
                mCurrentFrame.mdpToLayer[mdpIdx].listIndex = idx;
                mCurrentFrame.layerToMDP[idx] = mdpIdx++;
            }
        }
        //Acquire and Program MDP pipes
        mCurrentFrame.fbZ = programMDP(ctx, list);
    }

    /* Any change in composition types needs an FB refresh*/
    if(mCurrentFrame.fbCount &&
       ((mCurrentFrame.mdpCount != mCachedFrame.mdpCount) ||
       (mCurrentFrame.fbCount != mCachedFrame.cacheCount) ||
       !mCurrentFrame.mdpCount)) {
        mCurrentFrame.needsRedraw = true;
    }

    //UpdateLayerFlags
    setMDPCompLayerFlags(ctx, list);

    if(isDebug()) {
        android::String8 sDump("");
        dump(sDump);
        ALOGE("%s",sDump.string());
    }

    return mCurrentFrame.fbZ;
}

//=============MDPCompLowRes===================================================

/*
 * Configures pipe(s) for MDP composition
 */
int MDPCompLowRes::configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
                             PipeLayerPair& PipeLayerPair) {
    MdpPipeInfoLowRes& mdp_info =
        *(static_cast<MdpPipeInfoLowRes*>(PipeLayerPair.pipeInfo));
    eMdpFlags mdpFlags = OV_MDP_BACKEND_COMPOSITION;
    eZorder zOrder = static_cast<eZorder>(mdp_info.zOrder);
    eIsFg isFg = IS_FG_OFF;
    eDest dest = mdp_info.index;

    ALOGD_IF(isDebug(),"%s: configuring: layer: %p z_order: %d dest_pipe: %d",
             __FUNCTION__, layer, zOrder, dest);

    return configureLowRes(ctx, layer, mDpy, mdpFlags, zOrder, isFg, dest,
                           &PipeLayerPair.rot);
}

int MDPCompLowRes::pipesNeeded(hwc_context_t *ctx,
                               hwc_display_contents_1_t* list) {
    return mCurrentFrame.mdpCount;
}

bool MDPCompLowRes::allocLayerPipes(hwc_context_t *ctx,
                                    hwc_display_contents_1_t* list) {
    if(isYuvPresent(ctx, mDpy)) {
        int nYuvCount = ctx->listStats[mDpy].yuvCount;

        for(int index = 0; index < nYuvCount ; index ++) {
            int nYuvIndex = ctx->listStats[mDpy].yuvIndices[index];

            if(mCurrentFrame.isFBComposed[nYuvIndex])
                continue;

            hwc_layer_1_t* layer = &list->hwLayers[nYuvIndex];

            int mdpIndex = mCurrentFrame.layerToMDP[nYuvIndex];

            PipeLayerPair& info = mCurrentFrame.mdpToLayer[mdpIndex];
            info.pipeInfo = new MdpPipeInfoLowRes;
            info.rot = NULL;
            MdpPipeInfoLowRes& pipe_info = *(MdpPipeInfoLowRes*)info.pipeInfo;

            pipe_info.index = getMdpPipe(ctx, MDPCOMP_OV_VG);
            if(pipe_info.index == ovutils::OV_INVALID) {
                ALOGD_IF(isDebug(), "%s: Unable to get pipe for Videos",
                         __FUNCTION__);
                return false;
            }
        }
    }

    for(int index = 0 ; index < mCurrentFrame.layerCount; index++ ) {
        if(mCurrentFrame.isFBComposed[index]) continue;
        hwc_layer_1_t* layer = &list->hwLayers[index];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        if(isYuvBuffer(hnd))
            continue;

        int mdpIndex = mCurrentFrame.layerToMDP[index];

        PipeLayerPair& info = mCurrentFrame.mdpToLayer[mdpIndex];
        info.pipeInfo = new MdpPipeInfoLowRes;
        info.rot = NULL;
        MdpPipeInfoLowRes& pipe_info = *(MdpPipeInfoLowRes*)info.pipeInfo;

        ePipeType type = MDPCOMP_OV_ANY;

        if(!qhwc::needsScaling(layer) && !ctx->mNeedsRotator
           && ctx->mMDP.version >= qdutils::MDSS_V5) {
            type = MDPCOMP_OV_DMA;
        }

        pipe_info.index = getMdpPipe(ctx, type);
        if(pipe_info.index == ovutils::OV_INVALID) {
            ALOGD_IF(isDebug(), "%s: Unable to get pipe for UI", __FUNCTION__);
            return false;
        }
    }
    return true;
}

bool MDPCompLowRes::draw(hwc_context_t *ctx, hwc_display_contents_1_t* list) {

    if(!isEnabled()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp not configured", __FUNCTION__);
        return true;
    }

    if(!ctx || !list) {
        ALOGE("%s: invalid contxt or list",__FUNCTION__);
        return false;
    }

    /* reset Invalidator */
    if(idleInvalidator && mCurrentFrame.mdpCount)
        idleInvalidator->markForSleep();

    overlay::Overlay& ov = *ctx->mOverlay;
    LayerProp *layerProp = ctx->layerProp[mDpy];

    int numHwLayers = ctx->listStats[mDpy].numAppLayers;
    for(int i = 0; i < numHwLayers && mCurrentFrame.mdpCount; i++ )
    {
        if(mCurrentFrame.isFBComposed[i]) continue;

        hwc_layer_1_t *layer = &list->hwLayers[i];
        private_handle_t *hnd = (private_handle_t *)layer->handle;
        if(!hnd) {
            ALOGE("%s handle null", __FUNCTION__);
            return false;
        }

        int mdpIndex = mCurrentFrame.layerToMDP[i];

        MdpPipeInfoLowRes& pipe_info =
            *(MdpPipeInfoLowRes*)mCurrentFrame.mdpToLayer[mdpIndex].pipeInfo;
        ovutils::eDest dest = pipe_info.index;
        if(dest == ovutils::OV_INVALID) {
            ALOGE("%s: Invalid pipe index (%d)", __FUNCTION__, dest);
            return false;
        }

        if(!(layerProp[i].mFlags & HWC_MDPCOMP)) {
            continue;
        }

        ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                 using  pipe: %d", __FUNCTION__, layer,
                 hnd, dest );

        int fd = hnd->fd;
        uint32_t offset = hnd->offset;
        Rotator *rot = mCurrentFrame.mdpToLayer[mdpIndex].rot;
        if(rot) {
            if(!rot->queueBuffer(fd, offset))
                return false;
            fd = rot->getDstMemId();
            offset = rot->getDstOffset();
        }

        if (!ov.queueBuffer(fd, offset, dest)) {
            ALOGE("%s: queueBuffer failed for external", __FUNCTION__);
            return false;
        }

        layerProp[i].mFlags &= ~HWC_MDPCOMP;
    }
    return true;
}

//=============MDPCompHighRes===================================================

int MDPCompHighRes::pipesNeeded(hwc_context_t *ctx,
                                hwc_display_contents_1_t* list) {
    int pipesNeeded = 0;
    int hw_w = ctx->dpyAttr[mDpy].xres;

    for(int i = 0; i < mCurrentFrame.layerCount; ++i) {
        if(!mCurrentFrame.isFBComposed[i]) {
            hwc_layer_1_t* layer = &list->hwLayers[i];
            hwc_rect_t dst = layer->displayFrame;
            if(dst.left > hw_w/2) {
                pipesNeeded++;
            } else if(dst.right <= hw_w/2) {
                pipesNeeded++;
            } else {
                pipesNeeded += 2;
            }
        }
    }
    return pipesNeeded;
}

bool MDPCompHighRes::acquireMDPPipes(hwc_context_t *ctx, hwc_layer_1_t* layer,
                                     MdpPipeInfoHighRes& pipe_info,
                                     ePipeType type) {
    int hw_w = ctx->dpyAttr[mDpy].xres;

    hwc_rect_t dst = layer->displayFrame;
    if(dst.left > hw_w/2) {
        pipe_info.lIndex = ovutils::OV_INVALID;
        pipe_info.rIndex = getMdpPipe(ctx, type);
        if(pipe_info.rIndex == ovutils::OV_INVALID)
            return false;
    } else if (dst.right <= hw_w/2) {
        pipe_info.rIndex = ovutils::OV_INVALID;
        pipe_info.lIndex = getMdpPipe(ctx, type);
        if(pipe_info.lIndex == ovutils::OV_INVALID)
            return false;
    } else {
        pipe_info.rIndex = getMdpPipe(ctx, type);
        pipe_info.lIndex = getMdpPipe(ctx, type);
        if(pipe_info.rIndex == ovutils::OV_INVALID ||
           pipe_info.lIndex == ovutils::OV_INVALID)
            return false;
    }
    return true;
}

bool MDPCompHighRes::allocLayerPipes(hwc_context_t *ctx,
                                     hwc_display_contents_1_t* list) {
    overlay::Overlay& ov = *ctx->mOverlay;
    int layer_count = ctx->listStats[mDpy].numAppLayers;

    if(isYuvPresent(ctx, mDpy)) {
        int nYuvCount = ctx->listStats[mDpy].yuvCount;

        for(int index = 0; index < nYuvCount; index ++) {
            int nYuvIndex = ctx->listStats[mDpy].yuvIndices[index];
            hwc_layer_1_t* layer = &list->hwLayers[nYuvIndex];
            PipeLayerPair& info = mCurrentFrame.mdpToLayer[nYuvIndex];
            info.pipeInfo = new MdpPipeInfoHighRes;
            MdpPipeInfoHighRes& pipe_info = *(MdpPipeInfoHighRes*)info.pipeInfo;
            if(!acquireMDPPipes(ctx, layer, pipe_info,MDPCOMP_OV_VG)) {
                ALOGD_IF(isDebug(),"%s: Unable to get pipe for videos",
                         __FUNCTION__);
                //TODO: windback pipebook data on fail
                return false;
            }
            pipe_info.zOrder = nYuvIndex;
        }
    }

    for(int index = 0 ; index < layer_count ; index++ ) {
        hwc_layer_1_t* layer = &list->hwLayers[index];
        private_handle_t *hnd = (private_handle_t *)layer->handle;

        if(isYuvBuffer(hnd))
            continue;

        PipeLayerPair& info = mCurrentFrame.mdpToLayer[index];
        info.pipeInfo = new MdpPipeInfoHighRes;
        MdpPipeInfoHighRes& pipe_info = *(MdpPipeInfoHighRes*)info.pipeInfo;

        ePipeType type = MDPCOMP_OV_ANY;

        if(!qhwc::needsScaling(layer) && !ctx->mNeedsRotator
           && ctx->mMDP.version >= qdutils::MDSS_V5)
            type = MDPCOMP_OV_DMA;

        if(!acquireMDPPipes(ctx, layer, pipe_info, type)) {
            ALOGD_IF(isDebug(), "%s: Unable to get pipe for UI", __FUNCTION__);
            //TODO: windback pipebook data on fail
            return false;
        }
        pipe_info.zOrder = index;
    }
    return true;
}
/*
 * Configures pipe(s) for MDP composition
 */
int MDPCompHighRes::configure(hwc_context_t *ctx, hwc_layer_1_t *layer,
                              PipeLayerPair& PipeLayerPair) {
    MdpPipeInfoHighRes& mdp_info =
        *(static_cast<MdpPipeInfoHighRes*>(PipeLayerPair.pipeInfo));
    eZorder zOrder = static_cast<eZorder>(mdp_info.zOrder);
    eIsFg isFg = IS_FG_OFF;
    eMdpFlags mdpFlagsL = OV_MDP_BACKEND_COMPOSITION;
    eDest lDest = mdp_info.lIndex;
    eDest rDest = mdp_info.rIndex;

    ALOGD_IF(isDebug(),"%s: configuring: layer: %p z_order: %d dest_pipeL: %d"
             "dest_pipeR: %d",__FUNCTION__, layer, zOrder, lDest, rDest);

    return configureHighRes(ctx, layer, mDpy, mdpFlagsL, zOrder, isFg, lDest,
                            rDest, &PipeLayerPair.rot);
}

bool MDPCompHighRes::draw(hwc_context_t *ctx, hwc_display_contents_1_t* list) {

    if(!isEnabled()) {
        ALOGD_IF(isDebug(),"%s: MDP Comp not configured", __FUNCTION__);
        return true;
    }

    if(!ctx || !list) {
        ALOGE("%s: invalid contxt or list",__FUNCTION__);
        return false;
    }

    /* reset Invalidator */
    if(idleInvalidator && mCurrentFrame.mdpCount)
        idleInvalidator->markForSleep();

    overlay::Overlay& ov = *ctx->mOverlay;
    LayerProp *layerProp = ctx->layerProp[mDpy];

    int numHwLayers = ctx->listStats[mDpy].numAppLayers;
    for(int i = 0; i < numHwLayers && mCurrentFrame.mdpCount; i++ )
    {
        if(mCurrentFrame.isFBComposed[i]) continue;

        hwc_layer_1_t *layer = &list->hwLayers[i];
        private_handle_t *hnd = (private_handle_t *)layer->handle;
        if(!hnd) {
            ALOGE("%s handle null", __FUNCTION__);
            return false;
        }

        if(!(layerProp[i].mFlags & HWC_MDPCOMP)) {
            continue;
        }

        int mdpIndex = mCurrentFrame.layerToMDP[i];

        MdpPipeInfoHighRes& pipe_info =
            *(MdpPipeInfoHighRes*)mCurrentFrame.mdpToLayer[mdpIndex].pipeInfo;
        Rotator *rot = mCurrentFrame.mdpToLayer[mdpIndex].rot;

        ovutils::eDest indexL = pipe_info.lIndex;
        ovutils::eDest indexR = pipe_info.rIndex;

        int fd = hnd->fd;
        int offset = hnd->offset;

        if(rot) {
            rot->queueBuffer(fd, offset);
            fd = rot->getDstMemId();
            offset = rot->getDstOffset();
        }

        //************* play left mixer **********
        if(indexL != ovutils::OV_INVALID) {
            ovutils::eDest destL = (ovutils::eDest)indexL;
            ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                     using  pipe: %d", __FUNCTION__, layer, hnd, indexL );
            if (!ov.queueBuffer(fd, offset, destL)) {
                ALOGE("%s: queueBuffer failed for left mixer", __FUNCTION__);
                return false;
            }
        }

        //************* play right mixer **********
        if(indexR != ovutils::OV_INVALID) {
            ovutils::eDest destR = (ovutils::eDest)indexR;
            ALOGD_IF(isDebug(),"%s: MDP Comp: Drawing layer: %p hnd: %p \
                     using  pipe: %d", __FUNCTION__, layer, hnd, indexR );
            if (!ov.queueBuffer(fd, offset, destR)) {
                ALOGE("%s: queueBuffer failed for right mixer", __FUNCTION__);
                return false;
            }
        }

        layerProp[i].mFlags &= ~HWC_MDPCOMP;
    }

    return true;
}
}; //namespace

