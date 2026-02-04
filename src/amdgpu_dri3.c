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
#include <drm/drm_fourcc.h>
#include <drm/amdgpu_drm.h>

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

static PixmapPtr amdgpu_dri3_pixmap_from_fd(ScreenPtr screen,
					    int fd,
					    CARD16 width,
					    CARD16 height,
					    CARD16 stride,
					    CARD8 depth,
					    CARD8 bpp)
{
	PixmapPtr pixmap;

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

	if (screen->SetSharedPixmapBacking(pixmap, (void*)(intptr_t)fd))
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
	int i;
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
	/* Try multi-plane import with modifier first */
	if (modifier != DRM_FORMAT_MOD_INVALID && num_fds > 1) {
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
		/* Single plane or no modifier - use GBM_BO_IMPORT_FD */
		struct gbm_import_fd_data import_data = { 0 };

		if (num_fds != 1)
			goto error;

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

	if (screen->SetSharedPixmapBacking(pixmap, (void*)(intptr_t)fds[0]))
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

	/* For non-GBM buffers, use the tiling info to derive the modifier.
	 * DRM_FORMAT_MOD_INVALID indicates no modifier (linear layout).
	 * TODO: Extract actual modifier from bo_info.metadata.tiling_info
	 * for AMDGPU tiled surfaces. This requires mapping the legacy
	 * tiling flags to DRM modifiers.
	 */
	*modifier = DRM_FORMAT_MOD_INVALID;

	if (bo->flags & AMDGPU_BO_FLAGS_GBM) {
		/* For GBM buffers, try to get the modifier from the GBM BO */
		uint64_t gbm_modifier = gbm_bo_get_modifier(bo->bo.gbm);
		if (gbm_modifier != DRM_FORMAT_MOD_INVALID)
			*modifier = gbm_modifier;
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

	*num_formats = sizeof(formats_arr) / sizeof(formats_arr[0]);
	*formats = (unsigned int *)formats_arr;
	return sizeof(formats_arr) / sizeof(formats_arr[0]);
}

/*
 * Returns supported modifiers for a given format.
 * This includes LINEAR (DRM_FORMAT_MOD_INVALID) and AMD-specific tiled modifiers.
 */
static int
amdgpu_dri3_get_modifiers(ScreenPtr screen, uint32_t format,
			   uint32_t *num_modifiers, uint64_t **modifiers)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	static uint64_t default_modifiers[] = {
		/* LINEAR - no tiling */
		DRM_FORMAT_MOD_INVALID,
	};
	static uint64_t amd_tiled_modifiers[] = {
		/* LINEAR - no tiling */
		DRM_FORMAT_MOD_INVALID,
		/* AMD GFX9 64K_S tiled */
		AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX9 |
			AMD_FMT_MOD_TILE_GFX9_64K_S,
		/* AMD GFX9 64K_D tiled */
		AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX9 |
			AMD_FMT_MOD_TILE_GFX9_64K_D,
	};
	static uint64_t amd_tiled_modifiers_gfx10[] = {
		/* LINEAR - no tiling */
		DRM_FORMAT_MOD_INVALID,
		/* AMD GFX10 64K_S tiled */
		AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX10 |
			AMD_FMT_MOD_TILE_GFX9_64K_S,
		/* AMD GFX10 64K_D tiled */
		AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX10 |
			AMD_FMT_MOD_TILE_GFX9_64K_D,
	};
	static uint64_t amd_tiled_modifiers_gfx12[] = {
		/* LINEAR - no tiling */
		DRM_FORMAT_MOD_INVALID,
		/* AMD GFX12 64K_2D tiled */
		AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX12 |
			AMD_FMT_MOD_TILE_GFX12_64K_2D,
	};
	uint64_t *mods;
	uint32_t count;
	int asic_family;

	/* Get the ASIC family to determine which modifiers to advertise */
	asic_family = info->family;

	/* Determine which set of modifiers to return based on ASIC family */
	if (asic_family >= AMDGPU_FAMILY_GC_12_0_0) {
		mods = amd_tiled_modifiers_gfx12;
		count = sizeof(amd_tiled_modifiers_gfx12) / sizeof(amd_tiled_modifiers_gfx12[0]);
	} else if (asic_family >= AMDGPU_FAMILY_NV) {
		/* Navi and newer (GFX10+) */
		mods = amd_tiled_modifiers_gfx10;
		count = sizeof(amd_tiled_modifiers_gfx10) / sizeof(amd_tiled_modifiers_gfx10[0]);
	} else if (asic_family >= AMDGPU_FAMILY_AI) {
		/* Vega and newer (GFX9+) */
		mods = amd_tiled_modifiers;
		count = sizeof(amd_tiled_modifiers) / sizeof(amd_tiled_modifiers[0]);
	} else {
		/* For older chips, only support LINEAR */
		mods = default_modifiers;
		count = sizeof(default_modifiers) / sizeof(default_modifiers[0]);
	}

	/* Allocate and copy modifiers */
	*modifiers = malloc(count * sizeof(uint64_t));
	if (!*modifiers)
		return 0;

	memcpy(*modifiers, mods, count * sizeof(uint64_t));
	*num_modifiers = count;

	return count;
}

static int
amdgpu_dri3_get_drawable_modifiers(DrawablePtr draw, uint32_t format,
				   uint32_t *num_modifiers, uint64_t **modifiers)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(draw->pScreen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	uint64_t *mods;
	uint32_t count;
	int asic_family;

	/* Get the ASIC family to determine which modifiers to advertise */
	asic_family = info->family;

	/* Determine which set of modifiers to return based on ASIC family.
	 * For drawables, we return the same modifiers as screen-level modifiers.
	 */
	if (asic_family >= AMDGPU_FAMILY_GC_12_0_0) {
		static uint64_t gfx12_mods[] = {
			DRM_FORMAT_MOD_INVALID,
			AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX12 |
				AMD_FMT_MOD_TILE_GFX12_64K_2D,
		};
		mods = gfx12_mods;
		count = sizeof(gfx12_mods) / sizeof(gfx12_mods[0]);
	} else if (asic_family >= AMDGPU_FAMILY_NV) {
		/* Navi and newer (GFX10+) */
		static uint64_t gfx10_mods[] = {
			DRM_FORMAT_MOD_INVALID,
			AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX10 |
				AMD_FMT_MOD_TILE_GFX9_64K_S,
			AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX10 |
				AMD_FMT_MOD_TILE_GFX9_64K_D,
		};
		mods = gfx10_mods;
		count = sizeof(gfx10_mods) / sizeof(gfx10_mods[0]);
	} else if (asic_family >= AMDGPU_FAMILY_AI) {
		/* Vega and newer (GFX9+) */
		static uint64_t gfx9_mods[] = {
			DRM_FORMAT_MOD_INVALID,
			AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX9 |
				AMD_FMT_MOD_TILE_GFX9_64K_S,
			AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX9 |
				AMD_FMT_MOD_TILE_GFX9_64K_D,
		};
		mods = gfx9_mods;
		count = sizeof(gfx9_mods) / sizeof(gfx9_mods[0]);
	} else {
		/* For older chips, only support LINEAR */
		static uint64_t default_mods[] = {
			DRM_FORMAT_MOD_INVALID,
		};
		mods = default_mods;
		count = sizeof(default_mods) / sizeof(default_mods[0]);
	}

	/* Allocate and copy modifiers */
	*modifiers = malloc(count * sizeof(uint64_t));
	if (!*modifiers)
		return FALSE;

	memcpy(*modifiers, mods, count * sizeof(uint64_t));
	*num_modifiers = count;

	return TRUE;
}

static dri3_screen_info_rec amdgpu_dri3_screen_info = {
	.version = 2,
	.open = amdgpu_dri3_open,
	.pixmap_from_fd = amdgpu_dri3_pixmap_from_fd,
	// Version 1.1
	.fd_from_pixmap = amdgpu_dri3_fd_from_pixmap,
	// Version 1.2
	.pixmap_from_fds = amdgpu_dri3_pixmap_from_fds,
	.fds_from_pixmap = amdgpu_dri3_fds_from_pixmap,
	.get_formats = amdgpu_dri3_get_formats,
	.get_modifiers = amdgpu_dri3_get_modifiers,
	.get_drawable_modifiers = amdgpu_dri3_get_drawable_modifiers
	// Version 1.4
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
