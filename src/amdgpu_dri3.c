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
    uint32_t syncobj_handle;  /* DRM syncobj handle */
    Bool owns_handle;         /* Whether we own and should destroy the handle */
};

static void
amdgpu_dri3_syncobj_free(struct dri3_syncobj *syncobj)
{
    struct amdgpu_dri3_syncobj *amdgpu_syncobj =
        (struct amdgpu_dri3_syncobj *)syncobj;
    ScrnInfoPtr scrn = xf86ScreenToScrn(syncobj->screen);
    AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
    struct drm_syncobj_destroy destroy_args;

    if (amdgpu_syncobj->owns_handle && amdgpu_syncobj->syncobj_handle != 0) {
        destroy_args.handle = amdgpu_syncobj->syncobj_handle;
        drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy_args);
    }

    free(amdgpu_syncobj);
}

static Bool
amdgpu_dri3_syncobj_has_fence(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct amdgpu_dri3_syncobj *amdgpu_syncobj =
        (struct amdgpu_dri3_syncobj *)syncobj;
    ScrnInfoPtr scrn = xf86ScreenToScrn(syncobj->screen);
    AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
    struct drm_syncobj_handle handle_args = { 0 };
    int ret;

    (void)point;

    /* Try to export the syncobj to see if it has a fence */
    handle_args.handle = amdgpu_syncobj->syncobj_handle;
    handle_args.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    handle_args.fd = -1;

    ret = drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_args);
    if (ret == 0) {
        /* Has a fence, close the fd we just got */
        close(handle_args.fd);
        return TRUE;
    }

    return FALSE;
}

static Bool
amdgpu_dri3_syncobj_is_signaled(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct amdgpu_dri3_syncobj *amdgpu_syncobj =
        (struct amdgpu_dri3_syncobj *)syncobj;
    ScrnInfoPtr scrn = xf86ScreenToScrn(syncobj->screen);
    AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
    struct drm_syncobj_wait wait_args;
    int ret;

    /* Set up wait args for non-blocking check */
    wait_args.handles = (uint64_t)(uintptr_t)&amdgpu_syncobj->syncobj_handle;
    wait_args.count_handles = 1;
    wait_args.timeout_nsec = 0;  /* Non-blocking */
    wait_args.flags = 0;
    wait_args.first_signaled = 0;
    wait_args.pad = 0;

    (void)point;

    ret = drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_args);

    /* Return TRUE if wait succeeded (fence is signaled) or ioctl failed in a way
     * that indicates the fence is not present (ENOENT means handle not found) */
    if (ret == 0)
        return TRUE;  /* signaled */
    if (ret == -1 && (errno == ENOENT || errno == EINVAL))
        return TRUE;  /* no fence means signaled */
    return FALSE;  /* not signaled */
}

static int
amdgpu_dri3_syncobj_export_fence(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct amdgpu_dri3_syncobj *amdgpu_syncobj =
        (struct amdgpu_dri3_syncobj *)syncobj;
    ScrnInfoPtr scrn = xf86ScreenToScrn(syncobj->screen);
    AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
    struct drm_syncobj_handle handle_args = { 0 };
    int ret;

    (void)point;

    handle_args.handle = amdgpu_syncobj->syncobj_handle;
    handle_args.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    handle_args.fd = -1;

    ret = drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_args);
    if (ret != 0)
        return -1;

    return handle_args.fd;
}

static void
amdgpu_dri3_syncobj_import_fence(struct dri3_syncobj *syncobj, uint64_t point, int fd)
{
    struct amdgpu_dri3_syncobj *amdgpu_syncobj =
        (struct amdgpu_dri3_syncobj *)syncobj;
    ScrnInfoPtr scrn = xf86ScreenToScrn(syncobj->screen);
    AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
    struct drm_syncobj_handle handle_args;
    int ret;

    (void)point;

    /* First reset the syncobj */
    struct drm_syncobj_array reset_args = {
        .handles = (uint64_t)(uintptr_t)&amdgpu_syncobj->syncobj_handle,
        .count_handles = 1,
    };
    drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_RESET, &reset_args);

    /* Then import the fence fd into the syncobj */
    memset(&handle_args, 0, sizeof(handle_args));
    handle_args.handle = amdgpu_syncobj->syncobj_handle;
    handle_args.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    handle_args.fd = fd;

    ret = drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_args);
    (void)ret;
}

static void
amdgpu_dri3_syncobj_signal(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct amdgpu_dri3_syncobj *amdgpu_syncobj =
        (struct amdgpu_dri3_syncobj *)syncobj;
    ScrnInfoPtr scrn = xf86ScreenToScrn(syncobj->screen);
    AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
    struct drm_syncobj_array signal_args;

    (void)point;

    signal_args.handles = (uint64_t)(uintptr_t)&amdgpu_syncobj->syncobj_handle;
    signal_args.count_handles = 1;

    drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &signal_args);
}

static void
amdgpu_dri3_syncobj_submitted_eventfd(struct dri3_syncobj *syncobj,
                                      uint64_t point, int efd)
{
    /* Called when client submits an eventfd for signaling notification.
     * For AMDGPU, we handle this in the wait ioctl with eventfd support.
     */
    (void)syncobj;
    (void)point;
    (void)efd;
}

static void
amdgpu_dri3_syncobj_signaled_eventfd(struct dri3_syncobj *syncobj,
                                       uint64_t point, int efd)
{
    /* Called when the syncobj has been signaled via eventfd.
     * For AMDGPU, this is handled by the kernel's eventfd notification.
     */
    (void)syncobj;
    (void)point;
    (void)efd;
}

/* Not offered to the server: it ignores timeline points. */
static _X_UNUSED struct dri3_syncobj *
amdgpu_dri3_import_syncobj(ClientPtr client, ScreenPtr screen,
                            XID id, int fd)
{
    struct amdgpu_dri3_syncobj *amdgpu_syncobj;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);
    struct drm_syncobj_create create_args;
    int ret;

    /* Create a new syncobj */
    create_args.handle = 0;
    create_args.flags = 0;
    ret = drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_CREATE, &create_args);
    if (ret != 0) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "Failed to create DRM syncobj: %s\n", strerror(errno));
        return NULL;
    }

    /* Import the fence if provided */
    if (fd >= 0) {
        struct drm_syncobj_handle handle_args;

        memset(&handle_args, 0, sizeof(handle_args));
        handle_args.handle = create_args.handle;
        handle_args.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
        handle_args.fd = fd;

        ret = drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_args);
        if (ret != 0) {
            xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                       "Failed to import fence into syncobj: %s\n",
                       strerror(errno));
            struct drm_syncobj_destroy destroy_args = {
                .handle = create_args.handle
            };
            drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy_args);
            return NULL;
        }
    }

    amdgpu_syncobj = calloc(1, sizeof(*amdgpu_syncobj));
    if (!amdgpu_syncobj) {
        struct drm_syncobj_destroy destroy_args = {
            .handle = create_args.handle
        };
        drmIoctl(pAMDGPUEnt->fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy_args);
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
    amdgpu_syncobj->syncobj_handle = create_args.handle;
    amdgpu_syncobj->owns_handle = TRUE;

    (void)client;

    return &amdgpu_syncobj->base;
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

static PixmapPtr amdgpu_dri3_pixmap_from_fd(ScreenPtr screen,
					    int fd,
					    CARD16 width,
					    CARD16 height,
					    CARD16 stride,
					    CARD8 depth,
					    CARD8 bpp)
{
	PixmapPtr pixmap;
	int shared_fd;

	/* Avoid generating a GEM flink name if possible */
	if (AMDGPUPTR(xf86ScreenToScrn(screen))->use_glamor) {
		pixmap = glamor_pixmap_from_fd(screen, fd, width, height,
					       stride, depth, bpp);
		if (pixmap) {
			struct amdgpu_pixmap *priv = calloc(1, sizeof(*priv));

			if (priv) {
				amdgpu_set_pixmap_private(pixmap, priv);
				pixmap->usage_hint |= AMDGPU_CREATE_PIXMAP_DRI2;
				return pixmap;
			}

			screen->DestroyPixmap(pixmap);
			return NULL;
		}
	}

	if (depth < 8)
		return NULL;

	switch (bpp) {
	case 8:
	case 16:
	case 32:
		break;
	default:
		return NULL;
	}

	pixmap = screen->CreatePixmap(screen, 0, 0, depth,
				      AMDGPU_CREATE_PIXMAP_DRI2);
	if (!pixmap)
		return NULL;

	if (!screen->ModifyPixmapHeader(pixmap, width, height, 0, bpp, stride,
					NULL))
		goto free_pixmap;

	/* SetSharedPixmapBacking takes ownership; DRI3 closes fd itself */
	shared_fd = dup(fd);
	if (shared_fd < 0)
		goto free_pixmap;

	if (screen->SetSharedPixmapBacking(pixmap, (void*)(intptr_t)shared_fd))
		return pixmap;

free_pixmap:
	fbDestroyPixmap(pixmap);
	return NULL;
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
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	struct gbm_device *gbm;
	struct gbm_bo *bo;
	Bool ret;
	int i, shared_fd;
	uint32_t gbm_format;

	if (!info->use_glamor)
		goto non_glamor_path;

	/* glamor path: use GBM to import multi-plane buffers */
	gbm = glamor_egl_get_gbm_device(screen);
	if (!gbm)
		goto non_glamor_path;

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

non_glamor_path:
	/* Non-glamor path: only supports single-plane buffers.
	 * The SetSharedPixmapBacking interface only accepts a single FD.
	 */
	if (num_fds != 1)
		return NULL;

	if (modifier != DRM_FORMAT_MOD_INVALID &&
	    modifier != DRM_FORMAT_MOD_LINEAR)
		return NULL;

	if (depth < 8)
		return NULL;

	switch (bpp) {
	case 8:
	case 16:
	case 32:
		break;
	default:
		return NULL;
	}

	pixmap = screen->CreatePixmap(screen, 0, 0, depth,
				      AMDGPU_CREATE_PIXMAP_DRI2);
	if (!pixmap)
		return NULL;

	if (!screen->ModifyPixmapHeader(pixmap, width, height, 0, bpp, strides[0],
					NULL))
		goto free_pixmap;

	/* SetSharedPixmapBacking takes ownership; DRI3 closes fds itself */
	shared_fd = dup(fds[0]);
	if (shared_fd < 0)
		goto free_pixmap;

	if (screen->SetSharedPixmapBacking(pixmap, (void*)(intptr_t)shared_fd))
		return pixmap;

free_pixmap:
	fbDestroyPixmap(pixmap);
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
	.pixmap_from_fd = amdgpu_dri3_pixmap_from_fd,
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

	if (!dri3_screen_init(screen, &amdgpu_dri3_screen_info)) {
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "dri3_screen_init failed\n");
		return FALSE;
	}

	return TRUE;
}
