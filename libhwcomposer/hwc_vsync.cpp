/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are
 * retained for attribution purposes only.

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

#include <cutils/properties.h>
#include <utils/Log.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/msm_mdp.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include "hwc_utils.h"
#include "string.h"
#include "external.h"

namespace qhwc {

#define HWC_VSYNC_THREAD_NAME "hwcVsyncThread"

int hwc_vsync_control(hwc_context_t* ctx, int dpy, int enable)
{
    int ret = 0;
    if(!ctx->vstate.fakevsync &&
       ioctl(ctx->dpyAttr[dpy].fd, MSMFB_OVERLAY_VSYNC_CTRL,
             &enable) < 0) {
        ALOGE("%s: vsync control failed. Dpy=%d, enable=%d : %s",
              __FUNCTION__, dpy, enable, strerror(errno));
        ret = -errno;
    }
    return ret;
}

static void *vsync_loop(void *param)
{
    const char* vsync_timestamp_fb0 = "/sys/class/graphics/fb0/vsync_event";
    const char* vsync_timestamp_fb1 = "/sys/class/graphics/fb1/vsync_event";
    int dpy = HWC_DISPLAY_PRIMARY;

    hwc_context_t * ctx = reinterpret_cast<hwc_context_t *>(param);

    char thread_name[64] = HWC_VSYNC_THREAD_NAME;
    prctl(PR_SET_NAME, (unsigned long) &thread_name, 0, 0, 0);
    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY +
                android::PRIORITY_MORE_FAVORABLE);

    const int MAX_DATA = 64;
    const int MAX_RETRY_COUNT = 100;
    static char vdata[MAX_DATA];

    uint64_t cur_timestamp=0;
    ssize_t len = -1;
    int fd_timestamp = -1;
    int ret = 0;
    bool fb1_vsync = false;
    bool logvsync = false;

    char property[PROPERTY_VALUE_MAX];
    if(property_get("debug.hwc.fakevsync", property, NULL) > 0) {
        if(atoi(property) == 1)
            ctx->vstate.fakevsync = true;
    }

    if(property_get("debug.hwc.logvsync", property, 0) > 0) {
        if(atoi(property) == 1)
            logvsync = true;
    }

    /* Currently read vsync timestamp from drivers
       e.g. VSYNC=41800875994
       */
    fd_timestamp = open(vsync_timestamp_fb0, O_RDONLY);
    if (fd_timestamp < 0) {
        ALOGE ("FATAL:%s:not able to open file:%s, %s",  __FUNCTION__,
               (fb1_vsync) ? vsync_timestamp_fb1 : vsync_timestamp_fb0,
               strerror(errno));
        ctx->vstate.fakevsync = true;
    }

    do {
        pthread_mutex_lock(&ctx->vstate.lock);
        while (ctx->vstate.enable == false) {
            pthread_cond_wait(&ctx->vstate.cond, &ctx->vstate.lock);
        }
        pthread_mutex_unlock(&ctx->vstate.lock);

        if (!ctx->vstate.fakevsync) {
            for(int i = 0; i < MAX_RETRY_COUNT; i++) {
                len = pread(fd_timestamp, vdata, MAX_DATA, 0);
                if(len < 0 && (errno == EAGAIN ||
                               errno == EINTR  ||
                               errno == EBUSY)) {
                    ALOGW("%s: vsync read: %s, retry (%d/%d).",
                          __FUNCTION__, strerror(errno), i, MAX_RETRY_COUNT);
                    continue;
                } else {
                    break;
                }
            }

            if (len < 0) {
                ALOGE ("FATAL:%s:not able to read file:%s, %s", __FUNCTION__,
                       vsync_timestamp_fb0, strerror(errno));
                close (fd_timestamp);
                ctx->vstate.fakevsync = true;
            }

            // extract timestamp
            const char *str = vdata;
            if (!strncmp(str, "VSYNC=", strlen("VSYNC="))) {
                cur_timestamp = strtoull(str + strlen("VSYNC="), NULL, 0);
            } else {
                ALOGE ("FATAL: %s: vsync timestamp not in correct format: [%s]",
                       __FUNCTION__,
                       str);
                ctx->vstate.fakevsync = true;
            }

        } else {
            usleep(16000);
            cur_timestamp = systemTime();
        }
        // send timestamp to HAL
        if(ctx->vstate.enable) {
            ALOGD_IF (logvsync, "%s: timestamp %llu sent to HWC for %s",
                      __FUNCTION__, cur_timestamp, "fb0");
            ctx->proc->vsync(ctx->proc, dpy, cur_timestamp);
        }

    } while (true);
    if(fd_timestamp >= 0)
        close (fd_timestamp);

    return NULL;
}

void init_vsync_thread(hwc_context_t* ctx)
{
    int ret;
    pthread_t vsync_thread;
    ALOGI("Initializing VSYNC Thread");
    ret = pthread_create(&vsync_thread, NULL, vsync_loop, (void*) ctx);
    if (ret) {
        ALOGE("%s: failed to create %s: %s", __FUNCTION__,
              HWC_VSYNC_THREAD_NAME, strerror(ret));
    }
}

}; //namespace
