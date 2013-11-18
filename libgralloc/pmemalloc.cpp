/*
 * Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Code Aurora Forum, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <cutils/log.h>
#include <errno.h>
#include <linux/android_pmem.h>
#include "gralloc_priv.h"
#include "pmemalloc.h"

using namespace gralloc;

#define PMEM_ADSP_DEVICE "/dev/pmem_adsp"

// Common functions between userspace
// and kernel allocators
static int getOpenFlags(bool uncached)
{
    if(uncached)
        return O_RDWR | O_SYNC;
    else
        return O_RDWR;
}

static int alignPmem(int fd, size_t size, int align) {
    struct pmem_allocation allocation;
    allocation.size = size;
    allocation.align = align;
    if (ioctl(fd, PMEM_ALLOCATE_ALIGNED, &allocation))
        return -errno;
    return 0;
}

static int cleanPmem(void *base, size_t size, int offset, int fd) {
    struct pmem_addr pmem_addr;
    pmem_addr.vaddr = (unsigned long) base;
    pmem_addr.offset = offset;
    pmem_addr.length = size;
    if (ioctl(fd, PMEM_CLEAN_INV_CACHES, &pmem_addr))
        return -errno;
    return 0;
}

//-------------- PmemAdspAlloc-----------------------//

int PmemAdspAlloc::alloc_buffer(alloc_data& data)
{
    int err, offset = 0;
    int openFlags = getOpenFlags(data.uncached);
    int size = data.size;

    int fd = open(PMEM_ADSP_DEVICE, openFlags, 0);
    if (fd < 0) {
        err = -errno;
        ALOGE("%s: Error opening %s", __FUNCTION__, PMEM_ADSP_DEVICE);
        return err;
    }

    if (data.align == 8192) {
        // Tile format buffers need physical alignment to 8K
        // Default page size does not need this ioctl
        err = alignPmem(fd, size, 8192);
        if (err < 0) {
            ALOGE("alignPmem failed");
        }
    }
    void* base = mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        err = -errno;
        ALOGE("%s: failed to map pmem fd: %s", PMEM_ADSP_DEVICE,
              strerror(errno));
        close(fd);
        return err;
    }
    memset(base, 0, size);
    clean_buffer((void*)((intptr_t) base + offset), size, offset, fd, 0);
    data.base = base;
    data.offset = 0;
    data.fd = fd;
    ALOGV("%s: Allocated buffer base:%p size:%d fd:%d",
          PMEM_ADSP_DEVICE, base, size, fd);
    return 0;

}

int PmemAdspAlloc::free_buffer(void* base, size_t size, int offset, int fd)
{
    ALOGV("%s: Freeing buffer base:%p size:%d fd:%d",
          PMEM_ADSP_DEVICE, base, size, fd);

    int err =  unmap_buffer(base, size, offset);
    close(fd);
    return err;
}

int PmemAdspAlloc::map_buffer(void **pBase, size_t size, int offset, int fd)
{
    int err = 0;
    void *base = mmap(0, size, PROT_READ| PROT_WRITE,
                      MAP_SHARED, fd, 0);
    *pBase = base;
    if(base == MAP_FAILED) {
        err = -errno;
        ALOGE("%s: Failed to map memory in the client: %s",
              PMEM_ADSP_DEVICE, strerror(errno));
    } else {
        ALOGV("%s: Mapped buffer base:%p size:%d, fd:%d",
              PMEM_ADSP_DEVICE, base, size, fd);
    }
    return err;

}

int PmemAdspAlloc::unmap_buffer(void *base, size_t size, int offset)
{
    int err = 0;
    if (munmap(base, size)) {
        err = -errno;
        ALOGW("%s: Error unmapping memory at %p: %s",
              PMEM_ADSP_DEVICE, base, strerror(err));
    }
    return err;

}
int PmemAdspAlloc::clean_buffer(void *base, size_t size, int offset, int fd, int op)
{
    return cleanPmem(base, size, offset, fd);
}
