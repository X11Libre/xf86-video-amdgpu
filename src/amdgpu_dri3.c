/*
 * Copyright © 2013-2014 Intel Corporation
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */
#include "config.h"
#include <xorg-server.h>

#include "amdgpu_drv.h"

#include "amdgpu_glamor.h"
#include "amdgpu_pixmap.h"
#include "dri3.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gbm.h>
#include <errno.h>
#include <libgen.h>
#include <string.h>
#include <drm_fourcc.h>
#include <amdgpu_drm.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>

#ifndef ETIME /* FreeBSD, DragonFly */
#define ETIME ETIMEDOUT
#endif

/* DRI3 Sync Object support (version 1.4) */

#include <xf86drm.h>

/**
 * Convert legacy AMDGPU tiling_info to DRM modifier.
 * \param tiling_info The legacy tiling info from amdgpu_bo_metadata
 * \param asic_family The GPU family identifier
 * \return The DRM modifier, or DRM_FORMAT_MOD_INVALID if no modifier applies
 */
static uint64_t
amdgpu_tiling_info_to_modifier(uint64_t tiling_info, int asic_family)
{
	/* Linear buffer - no tiling */
	if (tiling_info == 0)
		return DRM_FORMAT_MOD_INVALID;

	/* GFX12 and later use a different tiling format */
	if (asic_family >= AMDGPU_FAMILY_GC_12_0_0) {
#ifdef HAVE_AMD_FMT_MOD_TILE_VER_GFX12
		uint32_t swizzle_mode = AMDGPU_TILING_GET(tiling_info, GFX12_SWIZZLE_MODE);
		uint64_t modifier = AMD_FMT_MOD |
			AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX12);

		switch (swizzle_mode) {
		case 0:
			/* LINEAR */
			return DRM_FORMAT_MOD_INVALID;
		case 1:
			modifier |= AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX12_256B_2D);
			break;
		case 2:
			modifier |= AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX12_4K_2D);
			break;
		case 3:
			modifier |= AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX12_64K_2D);
			break;
		case 4:
			modifier |= AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX12_256K_2D);
			break;
		default:
			return DRM_FORMAT_MOD_INVALID;
		}

		return modifier;
#else
		/* GFX12 not supported by this libdrm version */
		return DRM_FORMAT_MOD_INVALID;
#endif
	}

	/* GFX9 - GFX11: Check for 64K tiled modes */
	if (asic_family >= AMDGPU_FAMILY_AI) {
		uint32_t swizzle_mode = AMDGPU_TILING_GET(tiling_info, SWIZZLE_MODE);
		uint64_t modifier;

		/* Determine tile version based on family */
		if (asic_family >= AMDGPU_FAMILY_GC_11_0_0) {
			modifier = AMD_FMT_MOD |
				AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX11);
		} else if (asic_family >= AMDGPU_FAMILY_NV) {
			modifier = AMD_FMT_MOD |
				AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX10);
		} else {
			modifier = AMD_FMT_MOD |
				AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9);
		}

		/* Map swizzle mode to tile type */
		switch (swizzle_mode) {
		case 9:
			modifier |= AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S);
			break;
		case 10:
			modifier |= AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_D);
			break;
		default:
			return DRM_FORMAT_MOD_INVALID;
		}

		return modifier;
	}

	/* Pre-GFX9: No modifier support for older chips */
	return DRM_FORMAT_MOD_INVALID;
}

struct amdgpu_dri3_syncobj {
    struct dri3_syncobj base;
    uint32_t syncobj_handle;  /* DRM timeline syncobj handle */
};

enum amdgpu_dri3_syncobj_op {
    SYNCOBJ_OP_IMPORT,
    SYNCOBJ_OP_WAIT,
    SYNCOBJ_OP_EXPORT_FENCE,
    SYNCOBJ_OP_IMPORT_FENCE,
    SYNCOBJ_OP_SIGNAL,
    SYNCOBJ_OP_EVENTFD,
    SYNCOBJ_OP_COUNT
};

/* The first failure of each operation is a warning, later ones are debug
 * messages: these run for every presented frame.
 */
static void
amdgpu_dri3_syncobj_warn(ScreenPtr screen, enum amdgpu_dri3_syncobj_op op,
                         const char *call, int err)
{
    static Bool warned[SYNCOBJ_OP_COUNT];
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);

    xf86DrvMsgVerb(scrn->scrnIndex, X_WARNING,
                   warned[op] ? AMDGPU_LOGLEVEL_DEBUG : 1,
                   "DRI3 sync object: %s failed: %s\n", call, strerror(err));
    warned[op] = TRUE;
}

static int
amdgpu_dri3_syncobj_drm_fd(struct dri3_syncobj *syncobj)
{
    return AMDGPUEntPriv(xf86ScreenToScrn(syncobj->screen))->fd;
}

static uint32_t
amdgpu_dri3_syncobj_handle(struct dri3_syncobj *syncobj)
{
    return ((struct amdgpu_dri3_syncobj *)syncobj)->syncobj_handle;
}

static void
amdgpu_dri3_syncobj_free(struct dri3_syncobj *syncobj)
{
    uint32_t handle = amdgpu_dri3_syncobj_handle(syncobj);

    if (handle)
        drmSyncobjDestroy(amdgpu_dri3_syncobj_drm_fd(syncobj), handle);

    free(syncobj);
}

/* TRUE once the point has a fence (WAIT_AVAILABLE) or has signalled
 * (WAIT_FOR_SUBMIT); ETIME just means "not yet".
 */
static Bool
amdgpu_dri3_syncobj_check(struct dri3_syncobj *syncobj, uint64_t point,
                          uint32_t flags)
{
    uint32_t handle = amdgpu_dri3_syncobj_handle(syncobj);

    if (drmSyncobjTimelineWait(amdgpu_dri3_syncobj_drm_fd(syncobj),
                               &handle, &point, 1, 0, flags, NULL) == 0)
        return TRUE;

    if (errno != ETIME)
        amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_WAIT,
                                 "drmSyncobjTimelineWait", errno);
    return FALSE;
}

static Bool
amdgpu_dri3_syncobj_has_fence(struct dri3_syncobj *syncobj, uint64_t point)
{
    return amdgpu_dri3_syncobj_check(syncobj, point,
                                     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
}

static Bool
amdgpu_dri3_syncobj_is_signaled(struct dri3_syncobj *syncobj, uint64_t point)
{
    return amdgpu_dri3_syncobj_check(syncobj, point,
                                     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);
}

/* Returns a sync_file fd for the point, owned by the caller, or -1. */
static int
amdgpu_dri3_syncobj_export_fence(struct dri3_syncobj *syncobj, uint64_t point)
{
    int drm_fd = amdgpu_dri3_syncobj_drm_fd(syncobj);
    uint32_t temp;
    int fd = -1;

    if (drmSyncobjCreate(drm_fd, 0, &temp)) {
        amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_EXPORT_FENCE,
                                 "drmSyncobjCreate", errno);
        return -1;
    }

    if (drmSyncobjTransfer(drm_fd, temp, 0,
                           amdgpu_dri3_syncobj_handle(syncobj), point, 0))
        amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_EXPORT_FENCE,
                                 "drmSyncobjTransfer", errno);
    else if (drmSyncobjExportSyncFile(drm_fd, temp, &fd)) {
        amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_EXPORT_FENCE,
                                 "drmSyncobjExportSyncFile", errno);
        fd = -1;
    }

    drmSyncobjDestroy(drm_fd, temp);
    return fd;
}

static void
amdgpu_dri3_syncobj_signal(struct dri3_syncobj *syncobj, uint64_t point)
{
    uint32_t handle = amdgpu_dri3_syncobj_handle(syncobj);

    if (drmSyncobjTimelineSignal(amdgpu_dri3_syncobj_drm_fd(syncobj),
                                 &handle, &point, 1))
        amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_SIGNAL,
                                 "drmSyncobjTimelineSignal", errno);
}

/* Attaches the sync_file fence_fd to the point; takes ownership of fence_fd.
 * fence_fd -1 means the work is already complete.
 */
static void
amdgpu_dri3_syncobj_import_fence(struct dri3_syncobj *syncobj, uint64_t point,
                                 int fence_fd)
{
    int drm_fd = amdgpu_dri3_syncobj_drm_fd(syncobj);
    struct pollfd pfd = { .fd = fence_fd, .events = POLLIN };
    uint32_t temp;
    int ret;

    if (fence_fd < 0) {
        amdgpu_dri3_syncobj_signal(syncobj, point);
        return;
    }

    ret = drmSyncobjCreate(drm_fd, 0, &temp);
    if (ret) {
        amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_IMPORT_FENCE,
                                 "drmSyncobjCreate", errno);
    } else {
        ret = drmSyncobjImportSyncFile(drm_fd, temp, fence_fd);
        if (ret)
            amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_IMPORT_FENCE,
                                     "drmSyncobjImportSyncFile", errno);
        else {
            ret = drmSyncobjTransfer(drm_fd, amdgpu_dri3_syncobj_handle(syncobj),
                                     point, temp, 0, 0);
            if (ret)
                amdgpu_dri3_syncobj_warn(syncobj->screen,
                                         SYNCOBJ_OP_IMPORT_FENCE,
                                         "drmSyncobjTransfer", errno);
        }
        drmSyncobjDestroy(drm_fd, temp);
    }

    if (ret) {
        /* The client must still see the point signalled: wait for the
         * fence on the CPU instead.
         */
        while (poll(&pfd, 1, -1) < 0 && (errno == EINTR || errno == EAGAIN))
            ;
        amdgpu_dri3_syncobj_signal(syncobj, point);
    }

    close(fence_fd);
}

static void
amdgpu_dri3_syncobj_eventfd(struct dri3_syncobj *syncobj, uint64_t point,
                            int efd, uint32_t flags)
{
    if (drmSyncobjEventfd(amdgpu_dri3_syncobj_drm_fd(syncobj),
                          amdgpu_dri3_syncobj_handle(syncobj), point, efd,
                          flags) == 0)
        return;

    amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_EVENTFD,
                             "drmSyncobjEventfd", errno);
    /* Wake the waiter anyway; it checks the point again. */
    if (eventfd_write(efd, 1) != 0)
        amdgpu_dri3_syncobj_warn(syncobj->screen, SYNCOBJ_OP_EVENTFD,
                                 "eventfd_write", errno);
}

static void
amdgpu_dri3_syncobj_submitted_eventfd(struct dri3_syncobj *syncobj,
                                      uint64_t point, int efd)
{
    amdgpu_dri3_syncobj_eventfd(syncobj, point, efd,
                                DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
}

static void
amdgpu_dri3_syncobj_signaled_eventfd(struct dri3_syncobj *syncobj,
                                     uint64_t point, int efd)
{
    amdgpu_dri3_syncobj_eventfd(syncobj, point, efd, 0);
}

/* fd is the client's DRM timeline syncobj; the server closes it. */
static struct dri3_syncobj *
amdgpu_dri3_import_syncobj(ClientPtr client, ScreenPtr screen,
                            XID id, int fd)
{
    struct amdgpu_dri3_syncobj *amdgpu_syncobj;
    int drm_fd = AMDGPUEntPriv(xf86ScreenToScrn(screen))->fd;
    uint32_t handle;

    if (drmSyncobjFDToHandle(drm_fd, fd, &handle)) {
        amdgpu_dri3_syncobj_warn(screen, SYNCOBJ_OP_IMPORT,
                                 "drmSyncobjFDToHandle", errno);
        return NULL;
    }

    amdgpu_syncobj = calloc(1, sizeof(*amdgpu_syncobj));
    if (!amdgpu_syncobj) {
        drmSyncobjDestroy(drm_fd, handle);
        return NULL;
    }

    amdgpu_syncobj->base.id = id;
    amdgpu_syncobj->base.screen = screen;
    amdgpu_syncobj->base.refcount = 1;
    amdgpu_syncobj->base.free = amdgpu_dri3_syncobj_free;
    amdgpu_syncobj->base.has_fence = amdgpu_dri3_syncobj_has_fence;
    amdgpu_syncobj->base.is_signaled = amdgpu_dri3_syncobj_is_signaled;
    amdgpu_syncobj->base.export_fence = amdgpu_dri3_syncobj_export_fence;
    amdgpu_syncobj->base.import_fence = amdgpu_dri3_syncobj_import_fence;
    amdgpu_syncobj->base.signal = amdgpu_dri3_syncobj_signal;
    amdgpu_syncobj->base.submitted_eventfd = amdgpu_dri3_syncobj_submitted_eventfd;
    amdgpu_syncobj->base.signaled_eventfd = amdgpu_dri3_syncobj_signaled_eventfd;
    amdgpu_syncobj->syncobj_handle = handle;

    (void)client;

    return &amdgpu_syncobj->base;
}

/* Timeline syncobjs with eventfd notification are a property of the amdgpu
 * kernel driver, so one probe serves every screen. Usable before DRI3 init.
 */
Bool
amdgpu_dri3_syncobj_supported(ScrnInfoPtr scrn)
{
    static int supported = -1;
    int drm_fd = AMDGPUEntPriv(scrn)->fd;
    const char *reason = NULL;
    uint64_t cap = 0;

    if (supported >= 0)
        return supported;

    if (drmGetCap(drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap) {
        reason = "no timeline syncobjs";
    } else {
        /* An invalid handle gives ENOENT only if eventfd is supported. */
        errno = 0;
        if (drmSyncobjEventfd(drm_fd, 0, 0, -1, 0) == 0 || errno != ENOENT)
            reason = "no syncobj eventfd";
    }

    supported = reason == NULL;
    if (supported)
        xf86DrvMsg(scrn->scrnIndex, X_INFO, "DRI3 sync objects: enabled\n");
    else
        xf86DrvMsg(scrn->scrnIndex, X_INFO,
                   "DRI3 sync objects: disabled (%s)\n", reason);

    return supported;
}

static int open_card_node(ScreenPtr screen, int *out);
static int open_render_node(ScreenPtr screen, int *out);
static int amdgpu_dri3_open(ScreenPtr screen, RRProviderPtr provider, int *out);

static int
amdgpu_dri3_open_client(ClientPtr client, ScreenPtr screen,
			 RRProviderPtr provider, int *out)
{
	return amdgpu_dri3_open(screen, provider, out);
}

static int
amdgpu_dri3_open(ScreenPtr screen, RRProviderPtr provider, int *out)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
	int ret = BadAlloc;

	if (pAMDGPUEnt->render_node)
		ret = open_render_node(screen, out);

	if (ret != Success)
		ret = open_card_node(screen, out);

	return ret;
}

static int open_card_node(ScreenPtr screen, int *out)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	drm_magic_t magic;
	int fd;

	fd = open(info->dri2.device_name, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return BadAlloc;

	/* Before FD passing in the X protocol with DRI3 (and increased
	 * security of rendering with per-process address spaces on the
	 * GPU), the kernel had to come up with a way to have the server
	 * decide which clients got to access the GPU, which was done by
	 * each client getting a unique (magic) number from the kernel,
	 * passing it to the server, and the server then telling the
	 * kernel which clients were authenticated for using the device.
	 *
	 * Now that we have FD passing, the server can just set up the
	 * authentication on its own and hand the prepared FD off to the
	 * client.
	 */
	if (drmGetMagic(fd, &magic) < 0) {
		if (errno == EACCES) {
			/* Assume that we're on a render node, and the fd is
			 * already as authenticated as it should be.
			 */
			*out = fd;
			return Success;
		} else {
			close(fd);
			return BadMatch;
		}
	}

	if (drmAuthMagic(pAMDGPUEnt->fd, magic) < 0) {
		close(fd);
		return BadMatch;
	}

	*out = fd;
	return Success;
}

static int open_render_node(ScreenPtr screen, int *out)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
	int fd;

	fd = open(pAMDGPUEnt->render_node, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return BadAlloc;

	*out = fd;
	return Success;
}

/* Map X depth to GBM format, returns FALSE on failure */
static Bool
gbm_format_from_depth(CARD8 depth, uint32_t *gbm_format)
{
	switch (depth) {
	case 15:
		*gbm_format = GBM_FORMAT_ARGB1555;
		return TRUE;
	case 16:
		*gbm_format = GBM_FORMAT_RGB565;
		return TRUE;
	case 24:
		*gbm_format = GBM_FORMAT_XRGB8888;
		return TRUE;
	case 30:
		*gbm_format = GBM_FORMAT_ARGB2101010;
		return TRUE;
	case 32:
		*gbm_format = GBM_FORMAT_ARGB8888;
		return TRUE;
	default:
		return FALSE;
	}
}

static PixmapPtr amdgpu_dri3_pixmap_from_fds(ScreenPtr screen,
					    CARD8 num_fds,
					    const int *fds,
					    CARD16 width,
					    CARD16 height,
					    const CARD32 *strides,
					    const CARD32 *offsets,
					    CARD8 depth,
					    CARD8 bpp,
					    CARD64 modifier)
{
	PixmapPtr pixmap;
	struct gbm_device *gbm;
	struct gbm_bo *bo;
	Bool ret;
	int i;
	uint32_t gbm_format;

	/* DRI3 is only enabled with glamor: use GBM to import the buffers */
	gbm = glamor_egl_get_gbm_device(screen);
	if (!gbm)
		return NULL;

	pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
	if (!pixmap)
		return NULL;

	/* Map X depth to GBM format */
	if (!gbm_format_from_depth(depth, &gbm_format))
		goto error;

#ifdef GBM_BO_IMPORT_FD_MODIFIER
	/* GBM_BO_IMPORT_FD drops the modifier, so use it only without one */
	if (modifier != DRM_FORMAT_MOD_INVALID) {
		struct gbm_import_fd_modifier_data import_data = { 0 };

		import_data.width = width;
		import_data.height = height;
		import_data.format = gbm_format;
		import_data.num_fds = num_fds;
		import_data.modifier = modifier;
		for (i = 0; i < num_fds; i++) {
			import_data.fds[i] = fds[i];
			import_data.strides[i] = strides[i];
			import_data.offsets[i] = offsets[i];
		}
		bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &import_data,
				  GBM_BO_USE_RENDERING);
	} else
#endif
	{
		struct gbm_import_fd_data import_data = { 0 };

		if (num_fds != 1)
			goto error;
#ifndef GBM_BO_IMPORT_FD_MODIFIER
		if (modifier != DRM_FORMAT_MOD_INVALID &&
		    modifier != DRM_FORMAT_MOD_LINEAR)
			goto error;
#endif

		import_data.fd = fds[0];
		import_data.width = width;
		import_data.height = height;
		import_data.stride = strides[0];
		import_data.format = gbm_format;

		bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &import_data,
				  GBM_BO_USE_RENDERING);
	}

	if (bo) {
		screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, strides[0], NULL);
		ret = glamor_egl_create_textured_pixmap_from_gbm_bo(pixmap, bo, FALSE);
		gbm_bo_destroy(bo);
		if (ret) {
			struct amdgpu_pixmap *priv = calloc(1, sizeof(*priv));

			if (priv) {
				priv->has_modifier = modifier != DRM_FORMAT_MOD_INVALID;
				priv->modifier = modifier;
				amdgpu_set_pixmap_private(pixmap, priv);
				pixmap->usage_hint |= AMDGPU_CREATE_PIXMAP_DRI2;
				return pixmap;
			}

			screen->DestroyPixmap(pixmap);
			return NULL;
		}
	}

error:
	if (pixmap)
		dixDestroyPixmap(pixmap, 0);
	return NULL;
}

static int amdgpu_dri3_fd_from_pixmap(ScreenPtr screen,
				      PixmapPtr pixmap,
				      CARD16 *stride,
				      CARD32 *size)
{
	struct amdgpu_buffer *bo;
	struct amdgpu_bo_info bo_info;
	uint32_t fd;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);

	if (info->use_glamor) {
		int ret = glamor_fd_from_pixmap(screen, pixmap, stride, size);

		/* Any pending drawing operations need to be flushed to the
		 * kernel driver before the client starts using the pixmap
		 * storage for direct rendering.
		 */
		if (ret >= 0)
			amdgpu_glamor_flush(scrn);

		return ret;
	}

	bo = amdgpu_get_pixmap_bo(pixmap);
	if (!bo)
		return -1;

	if (pixmap->devKind > UINT16_MAX)
		return -1;

	if (amdgpu_bo_query_info(bo->bo.amdgpu, &bo_info) != 0)
		return -1;

	if (amdgpu_bo_export(bo->bo.amdgpu, amdgpu_bo_handle_type_dma_buf_fd,
			     &fd) != 0)
		return -1;

	*stride = pixmap->devKind;
	*size = bo_info.alloc_size;
	return fd;
}

static int amdgpu_dri3_fds_from_pixmap(ScreenPtr screen,
				 PixmapPtr pixmap,
				 int *fds,
				 uint32_t *strides,
				 uint32_t *offsets,
				 uint64_t *modifier)
{
	struct amdgpu_buffer *bo;
	struct amdgpu_bo_info bo_info;
	uint32_t fd;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);

	if (info->use_glamor) {
		/* For glamor, we need to get the GBM BO to extract modifier info */
		CARD16 stride16;
		CARD32 size;
		int ret;

		ret = glamor_fd_from_pixmap(screen, pixmap, &stride16, &size);
		if (ret < 0)
			return -1;

		fds[0] = ret;
		strides[0] = stride16;
		offsets[0] = 0;

		/* Flush any pending drawing operations */
		amdgpu_glamor_flush(scrn);

		/* Try to get the modifier from the pixmap's GBM BO.
		 * Note: This is a best-effort attempt since glamor_fd_from_pixmap
		 * doesn't expose the GBM BO directly.
		 */
		*modifier = DRM_FORMAT_MOD_INVALID;

		return 1;
	}

	bo = amdgpu_get_pixmap_bo(pixmap);
	if (!bo)
		return -1;

	if (pixmap->devKind > UINT32_MAX)
		return -1;

	if (amdgpu_bo_query_info(bo->bo.amdgpu, &bo_info) != 0)
		return -1;

	if (amdgpu_bo_export(bo->bo.amdgpu, amdgpu_bo_handle_type_dma_buf_fd,
			     &fd) != 0)
		return -1;

	fds[0] = fd;
	strides[0] = pixmap->devKind;
	offsets[0] = 0;

	/* Extract modifier from tiling_info for non-GBM buffers,
	 * or use GBM BO modifier for GBM buffers.
	 */
	if (bo->flags & AMDGPU_BO_FLAGS_GBM) {
		/* For GBM buffers, get the modifier from the GBM BO */
		uint64_t gbm_modifier = gbm_bo_get_modifier(bo->bo.gbm);
		*modifier = (gbm_modifier != DRM_FORMAT_MOD_INVALID) ?
			gbm_modifier :
			amdgpu_tiling_info_to_modifier(bo_info.metadata.tiling_info,
							info->family);
	} else {
		/* For non-GBM buffers, derive modifier from legacy tiling info */
		*modifier = amdgpu_tiling_info_to_modifier(
			bo_info.metadata.tiling_info, info->family);
	}

	return 1;
}

static int
amdgpu_dri3_get_formats(ScreenPtr screen, unsigned int *num_formats,
		    unsigned int **formats)
{
	static const uint32_t formats_arr[] = {
		/* 32-bit formats */
		DRM_FORMAT_ARGB8888,
		DRM_FORMAT_XRGB8888,
		DRM_FORMAT_BGRA8888,
		DRM_FORMAT_BGRX8888,
		/* 24-bit formats */
		DRM_FORMAT_RGB888,
		DRM_FORMAT_BGR888,
		/* 16-bit formats */
		DRM_FORMAT_RGB565,
		DRM_FORMAT_BGR565,
		/* YUV 4:2:0 formats */
		DRM_FORMAT_NV12,
		DRM_FORMAT_YUV420,
		DRM_FORMAT_P010,
		/* YUV 4:2:2 formats */
		DRM_FORMAT_NV16,
		DRM_FORMAT_YUV422,
		/* YUV 4:4:4 formats */
		DRM_FORMAT_YUV444,
		DRM_FORMAT_XYUV8888,
		/* 10-bit formats */
		DRM_FORMAT_ARGB2101010,
		DRM_FORMAT_XRGB2101010,
		DRM_FORMAT_BGRA1010102,
		DRM_FORMAT_BGRX1010102,
		/* 16-bit alpha formats */
		DRM_FORMAT_RGBA5551,
		DRM_FORMAT_RGBA4444,
		/* 8-bit formats */
		DRM_FORMAT_RGB332,
		DRM_FORMAT_BGR233,
	};
	unsigned int count = sizeof(formats_arr) / sizeof(formats_arr[0]);

	*formats = malloc(count * sizeof(uint32_t));
	if (!*formats)
		return 0;

	memcpy(*formats, formats_arr, count * sizeof(uint32_t));
	*num_formats = count;
	return count;
}

/* Scanout planes take ARGB buffers through their opaque format */
static uint32_t
amdgpu_dri3_opaque_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return DRM_FORMAT_XRGB8888;
	case DRM_FORMAT_ARGB2101010:
		return DRM_FORMAT_XRGB2101010;
	default:
		return format;
	}
}

/*
 * Union of the modifiers the CRTCs' primary planes accept for format (only
 * enabled CRTCs if enabled_only). Returns the count; *mods is malloc()ed,
 * NULL when the count is 0.
 */
static uint32_t
amdgpu_dri3_scanout_modifiers(ScrnInfoPtr scrn, uint32_t format,
			      Bool enabled_only, uint64_t **mods)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
	uint32_t count = 0, i, j, k, l;

	*mods = NULL;
	format = amdgpu_dri3_opaque_format(format);

	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

		if (enabled_only && !crtc->enabled)
			continue;

		for (j = 0; j < drmmode_crtc->num_formats; j++) {
			drmmode_format_ptr f = &drmmode_crtc->formats[j];

			if (f->format != format)
				continue;

			for (k = 0; k < f->num_modifiers; k++) {
				uint64_t *grown;

				for (l = 0; l < count; l++)
					if ((*mods)[l] == f->modifiers[k])
						break;
				if (l < count)
					continue;

				grown = realloc(*mods, (count + 1) * sizeof(**mods));
				if (!grown)
					return count;
				*mods = grown;
				(*mods)[count++] = f->modifiers[k];
			}
		}
	}

	return count;
}

/*
 * FALSE only when the pixmap was imported with an explicit modifier that no
 * enabled CRTC's primary plane accepts for its format.
 */
Bool
amdgpu_dri3_pixmap_can_scanout(PixmapPtr pixmap)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(pixmap->drawable.pScreen);
	struct amdgpu_pixmap *priv = amdgpu_get_pixmap_private(pixmap);
	uint32_t format, count, i;
	uint64_t *mods;
	Bool found = FALSE;

	if (!priv || !priv->has_modifier ||
	    !gbm_format_from_depth(pixmap->drawable.depth, &format))
		return TRUE;

	count = amdgpu_dri3_scanout_modifiers(scrn, format, TRUE, &mods);
	if (count == 0)
		return TRUE;	/* the kernel lists no modifiers to compare with */

	for (i = 0; i < count && !found; i++)
		found = mods[i] == priv->modifier;
	free(mods);

	return found;
}

typedef Bool (*amdgpu_glamor_get_modifiers_proc)(ScreenPtr screen,
						  uint32_t format,
						  uint32_t *num_modifiers,
						  uint64_t **modifiers);

/*
 * Modifiers clients may use for buffers of this format: glamor's list (what
 * the GPU renders and samples) where glamor exports it (25.1 and later),
 * else LINEAR plus what the scanout planes accept.
 */
static int
amdgpu_dri3_get_modifiers(ScreenPtr screen, uint32_t format,
			   uint32_t *num_modifiers, uint64_t **modifiers)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	uint64_t *scanout;
	uint32_t num_scanout, count, i;

	*num_modifiers = 0;
	*modifiers = NULL;

	if (info->use_glamor) {
		amdgpu_glamor_get_modifiers_proc get_modifiers =
			(amdgpu_glamor_get_modifiers_proc)
			LoaderSymbol("glamor_get_modifiers");

		if (get_modifiers &&
		    get_modifiers(screen, format, num_modifiers, modifiers) &&
		    *num_modifiers > 0)
			return *num_modifiers;

		free(*modifiers);
		*num_modifiers = 0;
		*modifiers = NULL;
	}

	num_scanout = amdgpu_dri3_scanout_modifiers(scrn, format, FALSE, &scanout);
	*modifiers = malloc((num_scanout + 1) * sizeof(**modifiers));
	if (!*modifiers) {
		free(scanout);
		return 0;
	}

	count = 0;
	(*modifiers)[count++] = DRM_FORMAT_MOD_LINEAR;
	for (i = 0; i < num_scanout; i++) {
		if (scanout[i] != DRM_FORMAT_MOD_LINEAR &&
		    scanout[i] != DRM_FORMAT_MOD_INVALID)
			(*modifiers)[count++] = scanout[i];
	}
	free(scanout);

	*num_modifiers = count;
	return count;
}

/*
 * Modifiers for buffers of this drawable: the enabled CRTCs' scanout
 * modifiers when the drawable can be page flipped, else none, so the
 * client falls back to the screen-level list.
 */
static int
amdgpu_dri3_get_drawable_modifiers(DrawablePtr draw, uint32_t format,
				   uint32_t *num_modifiers, uint64_t **modifiers)
{
	ScreenPtr screen = draw->pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);

	*num_modifiers = 0;
	*modifiers = NULL;

	if (draw->type != DRAWABLE_WINDOW ||
	    draw->width != screen->width || draw->height != screen->height ||
	    !scrn->vtSema || !info->allowPageFlip ||
	    info->sprites_visible > 0 || info->drmmode.dri2_flipping)
		return TRUE;

	*num_modifiers = amdgpu_dri3_scanout_modifiers(scrn, format, TRUE,
						       modifiers);
	return TRUE;
}

static dri3_screen_info_rec amdgpu_dri3_screen_info = {
	.version = 4,
	/* Version 1 */
	.open = amdgpu_dri3_open,
	.open_client = amdgpu_dri3_open_client,
	/* Version 1.1 */
	.fd_from_pixmap = amdgpu_dri3_fd_from_pixmap,
	/* Version 1.2 */
	.pixmap_from_fds = amdgpu_dri3_pixmap_from_fds,
	.fds_from_pixmap = amdgpu_dri3_fds_from_pixmap,
	.get_formats = amdgpu_dri3_get_formats,
	.get_modifiers = amdgpu_dri3_get_modifiers,
	.get_drawable_modifiers = amdgpu_dri3_get_drawable_modifiers,
};

Bool
amdgpu_dri3_screen_init(ScreenPtr screen)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);

	pAMDGPUEnt->render_node = drmGetRenderDeviceNameFromFd(pAMDGPUEnt->fd);

	amdgpu_dri3_screen_info.import_syncobj =
		amdgpu_dri3_syncobj_supported(scrn) ? amdgpu_dri3_import_syncobj : NULL;

	if (!dri3_screen_init(screen, &amdgpu_dri3_screen_info)) {
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "dri3_screen_init failed\n");
		return FALSE;
	}

	return TRUE;
}
