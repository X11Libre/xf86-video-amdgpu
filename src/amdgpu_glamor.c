/*
 * Copyright © 2011 Intel Corporation.
 *             2012 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef USE_GLAMOR

#include <xf86.h>

#include "amdgpu_bo_helper.h"
#include "amdgpu_pixmap.h"
#include "amdgpu_glamor.h"

#include <gbm.h>

#ifndef HAVE_GLAMOR_FINISH
#include <GL/gl.h>
#endif

DevPrivateKeyRec amdgpu_pixmap_index;

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
void amdgpu_bind_glamor_api(void *glamor_module, AMDGPUInfoPtr info)
{
	info->glamor.init = LoaderSymbolFromModule(glamor_module, "glamor_init");
#ifdef HAVE_GLAMOR_GLYPHS_INIT
	info->glamor.glyphs_init = LoaderSymbolFromModule(glamor_module, "glamor_glyphs_init");
#endif
	info->glamor.xv_init = LoaderSymbolFromModule(glamor_module, "glamor_xv_init");
	info->glamor.egl_init = LoaderSymbolFromModule(glamor_module, "glamor_egl_init");
	info->glamor.egl_init_textured_pixmap = LoaderSymbolFromModule(glamor_module, "glamor_egl_init_textured_pixmap");
	info->glamor.egl_create_textured_pixmap = LoaderSymbolFromModule(glamor_module, "glamor_egl_create_textured_pixmap");
	info->glamor.egl_create_textured_pixmap_from_gbm_bo = LoaderSymbolFromModule(glamor_module, "glamor_egl_create_textured_pixmap_from_gbm_bo");
	info->glamor.egl_exchange_buffers = LoaderSymbolFromModule(glamor_module, "glamor_exchange_buffers");
#ifdef HAVE_GLAMOR_EGL_DESTROY_TEXTURED_PIXMAP
	info->glamor.egl_destroy_textured_pixmap = LoaderSymbolFromModule(glamor_module, "glamor_egl_destroy_textured_pixmap");
#endif
	info->glamor.create_pixmap = LoaderSymbolFromModule(glamor_module, "glamor_create_pixmap");
	info->glamor.pixmap_from_fd = LoaderSymbolFromModule(glamor_module, "glamor_pixmap_from_fd");
	info->glamor.fd_from_pixmap = LoaderSymbolFromModule(glamor_module, "glamor_fd_from_pixmap");
	info->glamor.validate_gc = LoaderSymbolFromModule(glamor_module, "glamor_validate_gc");
	info->glamor.block_handler = LoaderSymbolFromModule(glamor_module, "glamor_block_handler");
	info->glamor.finish = LoaderSymbolFromModule(glamor_module, "glamor_finish");
}
#endif

void amdgpu_glamor_exchange_buffers(PixmapPtr src, PixmapPtr dst)
{
	AMDGPUInfoPtr info = AMDGPUPTR(xf86ScreenToScrn(dst->drawable.pScreen));

	if (!info->use_glamor)
		return;
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
	info->glamor.egl_exchange_buffers(src, dst);
#else
	glamor_egl_exchange_buffers(src, dst);
#endif
}

Bool amdgpu_glamor_create_screen_resources(ScreenPtr screen)
{
	PixmapPtr screen_pixmap = screen->GetScreenPixmap(screen);
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);

	if (!info->use_glamor)
		return TRUE;

#ifdef HAVE_GLAMOR_GLYPHS_INIT
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
	if (!info->glamor.glyphs_init(screen))
#else
	if (!glamor_glyphs_init(screen))
#endif
		return FALSE;
#endif

	return amdgpu_glamor_create_textured_pixmap(screen_pixmap,
						    info->front_buffer);
}

Bool amdgpu_glamor_pre_init(ScrnInfoPtr scrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	pointer glamor_module;
	CARD32 version;

#if XORG_VERSION_CURRENT < XORG_VERSION_NUMERIC(1,20,99,0,0)
	if (scrn->depth < 24) {
#else
	if (scrn->depth < 15) {
#endif
		xf86DrvMsg(scrn->scrnIndex, X_ERROR,
			   "Depth %d not supported with glamor, disabling\n",
			   scrn->depth);
		return FALSE;
	}

	/* Load glamor module */
	if ((glamor_module = xf86LoadSubModule(scrn, GLAMOR_EGL_MODULE_NAME))) {
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
		amdgpu_bind_glamor_api(glamor_module, info);
#endif
		version = xf86GetModuleVersion(glamor_module);
		if (version < MODULE_VERSION_NUMERIC(0, 3, 1)) {
			xf86DrvMsg(scrn->scrnIndex, X_ERROR,
				   "Incompatible glamor version, required >= 0.3.0.\n");
			return FALSE;
		} else {
			AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(scrn);

			if (scrn->depth == 30 &&
			    version < MODULE_VERSION_NUMERIC(1, 0, 1)) {
				xf86DrvMsg(scrn->scrnIndex, X_WARNING,
					   "Depth 30 requires glamor >= 1.0.1 (xserver 1.20),"
					   " can't enable glamor\n");
				return FALSE;
			}

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
			if (info->glamor.egl_init(scrn, pAMDGPUEnt->fd)) {
#else
			if (glamor_egl_init(scrn, pAMDGPUEnt->fd)) {
#endif
				xf86DrvMsg(scrn->scrnIndex, X_INFO,
					   "glamor detected, initialising EGL layer.\n");
			} else {
				xf86DrvMsg(scrn->scrnIndex, X_ERROR,
					   "glamor detected, failed to initialize EGL.\n");
				return FALSE;
			}
		}
	} else {
		xf86DrvMsg(scrn->scrnIndex, X_ERROR, "glamor not available\n");
		return FALSE;
	}

	info->use_glamor = TRUE;

	return TRUE;
}

Bool
amdgpu_glamor_create_textured_pixmap(PixmapPtr pixmap, struct amdgpu_buffer *bo)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(pixmap->drawable.pScreen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);

	if ((info->use_glamor) == 0)
		return TRUE;

	if (bo->flags & AMDGPU_BO_FLAGS_GBM) {
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
		return info->glamor.egl_create_textured_pixmap_from_gbm_bo(pixmap,
#else
		return glamor_egl_create_textured_pixmap_from_gbm_bo(pixmap,
#endif
								     bo->bo.gbm
#if XORG_VERSION_CURRENT > XORG_VERSION_NUMERIC(1,19,99,903,0)
								     , FALSE
#endif
								     );
	} else {
		uint32_t bo_handle;

		if (!amdgpu_bo_get_handle(bo, &bo_handle))
			return FALSE;

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
		return info->glamor.egl_create_textured_pixmap(pixmap, bo_handle,
#else
		return glamor_egl_create_textured_pixmap(pixmap, bo_handle,
#endif
							 pixmap->devKind);
	}
}

static Bool amdgpu_glamor_destroy_pixmap(PixmapPtr pixmap)
{
#ifndef HAVE_GLAMOR_EGL_DESTROY_TEXTURED_PIXMAP
	ScreenPtr screen = pixmap->drawable.pScreen;
	AMDGPUInfoPtr info = AMDGPUPTR(xf86ScreenToScrn(screen));
	Bool ret = TRUE;
#endif

	if (pixmap->refcnt == 1) {
		if (pixmap->devPrivate.ptr) {
			struct amdgpu_buffer *bo = amdgpu_get_pixmap_bo(pixmap);

			if (bo)
				amdgpu_bo_unmap(bo);
		}

#ifdef HAVE_GLAMOR_EGL_DESTROY_TEXTURED_PIXMAP
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
		info->glamor.egl_destroy_textured_pixmap(pixmap);
#else
		glamor_egl_destroy_textured_pixmap(pixmap);
#endif
#endif
		amdgpu_set_pixmap_bo(pixmap, NULL);
	}

#ifdef HAVE_GLAMOR_EGL_DESTROY_TEXTURED_PIXMAP
	fbDestroyPixmap(pixmap);
	return TRUE;
#else
	screen->DestroyPixmap = info->glamor.SavedDestroyPixmap;
	if (screen->DestroyPixmap)
		ret = screen->DestroyPixmap(pixmap);
	info->glamor.SavedDestroyPixmap = screen->DestroyPixmap;
	screen->DestroyPixmap = amdgpu_glamor_destroy_pixmap;

	return ret;
#endif
}

static PixmapPtr
amdgpu_glamor_create_pixmap(ScreenPtr screen, int w, int h, int depth,
			    unsigned usage)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	PixmapFormatPtr format = xf86GetPixFormat(scrn, depth);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
	struct amdgpu_pixmap *priv;
	PixmapPtr pixmap, new_pixmap = NULL;

	if (!format)
		return NULL;

	if (usage != CREATE_PIXMAP_USAGE_BACKING_PIXMAP &&
	    usage != CREATE_PIXMAP_USAGE_SHARED &&
	    !info->shadow_primary &&
	    w >= scrn->virtualX &&
	    w <= scrn->displayWidth &&
	    h == scrn->virtualY &&
	    format->bitsPerPixel == scrn->bitsPerPixel)
		usage |= AMDGPU_CREATE_PIXMAP_SCANOUT;

	if (!(usage & AMDGPU_CREATE_PIXMAP_SCANOUT) &&
	    !AMDGPU_CREATE_PIXMAP_SHARED(usage)) {
		if (info->shadow_primary) {
			if (usage != CREATE_PIXMAP_USAGE_BACKING_PIXMAP)
				return fbCreatePixmap(screen, w, h, depth, usage);

			usage |= AMDGPU_CREATE_PIXMAP_LINEAR |
				 AMDGPU_CREATE_PIXMAP_GTT;
		} else if (usage != CREATE_PIXMAP_USAGE_BACKING_PIXMAP) {
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
			pixmap = info->glamor.create_pixmap(screen, w, h, depth, usage);
#else
			pixmap = glamor_create_pixmap(screen, w, h, depth, usage);
#endif
			if (pixmap)
				return pixmap;
		}
	}

	if (w > 32767 || h > 32767)
		return NullPixmap;

	if (depth == 1)
		return fbCreatePixmap(screen, w, h, depth, usage);

	if (usage == CREATE_PIXMAP_USAGE_GLYPH_PICTURE && w <= 32 && h <= 32)
		return fbCreatePixmap(screen, w, h, depth, usage);

	pixmap = fbCreatePixmap(screen, 0, 0, depth, usage);
	if (pixmap == NullPixmap)
		return pixmap;

	if (w && h) {
		int stride;

		priv = calloc(1, sizeof(struct amdgpu_pixmap));
		if (!priv)
			goto fallback_pixmap;

		priv->bo = amdgpu_alloc_pixmap_bo(scrn, w, h, depth, usage,
						  pixmap->drawable.bitsPerPixel,
						  &stride);
		if (!priv->bo)
			goto fallback_priv;

		amdgpu_set_pixmap_private(pixmap, priv);

		screen->ModifyPixmapHeader(pixmap, w, h, 0, 0, stride, NULL);

		pixmap->devPrivate.ptr = NULL;

		if (!amdgpu_glamor_create_textured_pixmap(pixmap, priv->bo))
			goto fallback_glamor;
	}

	return pixmap;

fallback_glamor:
	if (AMDGPU_CREATE_PIXMAP_SHARED(usage)) {
		/* XXX need further work to handle the DRI2 failure case.
		 * Glamor don't know how to handle a BO only pixmap. Put
		 * a warning indicator here.
		 */
		xf86DrvMsg(scrn->scrnIndex, X_WARNING,
			   "Failed to create textured DRI2/PRIME pixmap.");

		amdgpu_glamor_destroy_pixmap(pixmap);
		return NullPixmap;
	}
	/* Create textured pixmap failed means glamor failed to
	 * create a texture from current BO for some reasons. We turn
	 * to create a new glamor pixmap and clean up current one.
	 * One thing need to be noted, this new pixmap doesn't
	 * has a priv and bo attached to it. It's glamor's responsbility
	 * to take care of it. Glamor will mark this new pixmap as a
	 * texture only pixmap and will never fallback to DDX layer
	 * afterwards.
	 */
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
	new_pixmap = info->glamor.create_pixmap(screen, w, h, depth, usage);
#else
	new_pixmap = glamor_create_pixmap(screen, w, h, depth, usage);
#endif
	amdgpu_bo_unref(&priv->bo);
fallback_priv:
	free(priv);
fallback_pixmap:
	fbDestroyPixmap(pixmap);
	if (new_pixmap)
		return new_pixmap;
	else
		return fbCreatePixmap(screen, w, h, depth, usage);
}

PixmapPtr
amdgpu_glamor_set_pixmap_bo(DrawablePtr drawable, PixmapPtr pixmap)
{
	PixmapPtr old = get_drawable_pixmap(drawable);
	ScreenPtr screen = drawable->pScreen;
	struct amdgpu_pixmap *priv = amdgpu_get_pixmap_private(pixmap);
	GCPtr gc;

	/* With a glamor pixmap, 2D pixmaps are created in texture
	 * and without a static BO attached to it. To support DRI,
	 * we need to create a new textured-drm pixmap and
	 * need to copy the original content to this new textured-drm
	 * pixmap, and then convert the old pixmap to a coherent
	 * textured-drm pixmap which has a valid BO attached to it
	 * and also has a valid texture, thus both glamor and DRI2
	 * can access it.
	 *
	 */

	/* Copy the current contents of the pixmap to the bo. */
	gc = GetScratchGC(drawable->depth, screen);
	if (gc) {
		ValidateGC(&pixmap->drawable, gc);
		gc->ops->CopyArea(&old->drawable, &pixmap->drawable,
				  gc,
				  0, 0,
				  old->drawable.width,
				  old->drawable.height, 0, 0);
		FreeScratchGC(gc);
	}

	/* And redirect the pixmap to the new bo (for 3D). */
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
	AMDGPUInfoPtr info = AMDGPUPTR(xf86ScreenToScrn(screen));
	info->glamor.egl_exchange_buffers(old, pixmap);
#else
	glamor_egl_exchange_buffers(old, pixmap);
#endif
	amdgpu_set_pixmap_private(pixmap, amdgpu_get_pixmap_private(old));
	amdgpu_set_pixmap_private(old, priv);

	screen->ModifyPixmapHeader(old,
				   old->drawable.width,
				   old->drawable.height,
				   0, 0, pixmap->devKind, NULL);
	old->devPrivate.ptr = NULL;

	dixDestroyPixmap(pixmap, 0);

	return old;
}


static Bool
amdgpu_glamor_share_pixmap_backing(PixmapPtr pixmap, ScreenPtr secondary,
				   void **handle_p)
{
	ScreenPtr screen = pixmap->drawable.pScreen;
	AMDGPUInfoPtr info = AMDGPUPTR(xf86ScreenToScrn(screen));
	uint64_t tiling_info;
	CARD16 stride;
	CARD32 size;
	Bool is_linear;
	int fd;

	tiling_info = amdgpu_pixmap_get_tiling_info(pixmap);

	if (info->family >= AMDGPU_FAMILY_GC_12_0_0)
		is_linear = AMDGPU_TILING_GET(tiling_info, GFX12_SWIZZLE_MODE) == 0;
	else if (info->family >= AMDGPU_FAMILY_AI)
		is_linear = AMDGPU_TILING_GET(tiling_info, SWIZZLE_MODE) == 0;
	else
		is_linear = AMDGPU_TILING_GET(tiling_info, ARRAY_MODE) == 1;

	if (!is_linear) {
		PixmapPtr linear;

		/* We don't want to re-allocate the screen pixmap as
		 * linear, to avoid trouble with page flipping
		 */
		if (screen->GetScreenPixmap(screen) == pixmap)
			return FALSE;

		linear = screen->CreatePixmap(screen, pixmap->drawable.width,
					      pixmap->drawable.height,
					      pixmap->drawable.depth,
					      CREATE_PIXMAP_USAGE_SHARED);
		if (!linear)
			return FALSE;

		amdgpu_glamor_set_pixmap_bo(&pixmap->drawable, linear);
	}

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
	fd = info->glamor.fd_from_pixmap(screen, pixmap, &stride, &size);
#else
	fd = glamor_fd_from_pixmap(screen, pixmap, &stride, &size);
#endif
	if (fd < 0)
		return FALSE;

	*handle_p = (void *)(long)fd;
	return TRUE;
}

static Bool
amdgpu_glamor_set_shared_pixmap_backing(PixmapPtr pixmap, void *handle)
{
	ScreenPtr screen = pixmap->drawable.pScreen;
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	int ihandle = (int)(long)handle;
	struct amdgpu_pixmap *priv;

	if (!amdgpu_set_shared_pixmap_backing(pixmap, handle))
		return FALSE;

	priv = amdgpu_get_pixmap_private(pixmap);

	if (ihandle != -1 &&
	    !amdgpu_glamor_create_textured_pixmap(pixmap, priv->bo)) {
		xf86DrvMsg(scrn->scrnIndex, X_ERROR,
			   "Failed to get PRIME drawable for glamor pixmap.\n");
		return FALSE;
	}

	screen->ModifyPixmapHeader(pixmap,
				   pixmap->drawable.width,
				   pixmap->drawable.height,
				   0, 0, 0, NULL);

	return TRUE;
}


Bool amdgpu_glamor_init(ScreenPtr screen)
{
	ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(scrn);
#ifdef RENDER
#ifdef HAVE_FBGLYPHS
	UnrealizeGlyphProcPtr SavedUnrealizeGlyph = NULL;
#endif
	PictureScreenPtr ps = NULL;

	if (info->shadow_primary) {
		ps = GetPictureScreenIfSet(screen);

		if (ps) {
#ifdef HAVE_FBGLYPHS
			SavedUnrealizeGlyph = ps->UnrealizeGlyph;
#endif
			info->glamor.SavedGlyphs = ps->Glyphs;
			info->glamor.SavedTriangles = ps->Triangles;
			info->glamor.SavedTrapezoids = ps->Trapezoids;
		}
	}
#endif /* RENDER */

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
	if (!info->glamor.init(screen, GLAMOR_USE_EGL_SCREEN | GLAMOR_USE_SCREEN |
#else
	if (!glamor_init(screen, GLAMOR_USE_EGL_SCREEN | GLAMOR_USE_SCREEN |
#endif
			 GLAMOR_USE_PICTURE_SCREEN | GLAMOR_INVERTED_Y_AXIS |
			 GLAMOR_NO_DRI3)) {
		xf86DrvMsg(scrn->scrnIndex, X_ERROR,
			   "Failed to initialize glamor.\n");
		return FALSE;
	}

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
	if (!info->glamor.egl_init_textured_pixmap(screen)) {
#else
	if (!glamor_egl_init_textured_pixmap(screen)) {
#endif
		xf86DrvMsg(scrn->scrnIndex, X_ERROR,
			   "Failed to initialize textured pixmap of screen for glamor.\n");
		return FALSE;
	}
	if (!dixRegisterPrivateKey(&amdgpu_pixmap_index, PRIVATE_PIXMAP, 0))
		return FALSE;

	if (info->shadow_primary)
		amdgpu_glamor_screen_init(screen);

#if defined(RENDER) && defined(HAVE_FBGLYPHS)
	/* For ShadowPrimary, we need fbUnrealizeGlyph instead of
	 * glamor_unrealize_glyph
	 */
	if (ps)
		ps->UnrealizeGlyph = SavedUnrealizeGlyph;
#endif

	info->glamor.SavedCreatePixmap = screen->CreatePixmap;
	screen->CreatePixmap = amdgpu_glamor_create_pixmap;
	info->glamor.SavedDestroyPixmap = screen->DestroyPixmap;
	screen->DestroyPixmap = amdgpu_glamor_destroy_pixmap;
	info->glamor.SavedSharePixmapBacking = screen->SharePixmapBacking;
	screen->SharePixmapBacking = amdgpu_glamor_share_pixmap_backing;
	info->glamor.SavedSetSharedPixmapBacking = screen->SetSharedPixmapBacking;
	screen->SetSharedPixmapBacking =
	    amdgpu_glamor_set_shared_pixmap_backing;

	xf86DrvMsg(scrn->scrnIndex, X_INFO, "Use GLAMOR acceleration.\n");
	return TRUE;
}

void amdgpu_glamor_flush(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	if (info->use_glamor) {
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
		info->glamor.block_handler(pScrn->pScreen);
#else
		glamor_block_handler(pScrn->pScreen);
#endif
	}

	info->gpu_flushed++;
}

void amdgpu_glamor_finish(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	if (info->use_glamor) {
#if HAVE_GLAMOR_FINISH
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
		info->glamor.finish(pScrn->pScreen);
#else
		glamor_finish(pScrn->pScreen);
#endif
		info->gpu_flushed++;
#else
		amdgpu_glamor_flush(pScrn);
		glFinish();
#endif
	}
}

void
amdgpu_glamor_fini(ScreenPtr screen)
{
	AMDGPUInfoPtr info = AMDGPUPTR(xf86ScreenToScrn(screen));

	if (!info->use_glamor)
		return;

	screen->CreatePixmap = info->glamor.SavedCreatePixmap;
	screen->DestroyPixmap = info->glamor.SavedDestroyPixmap;
	screen->SharePixmapBacking = info->glamor.SavedSharePixmapBacking;
	screen->SetSharedPixmapBacking = info->glamor.SavedSetSharedPixmapBacking;
}

XF86VideoAdaptorPtr amdgpu_glamor_xv_init(ScreenPtr pScreen, int num_adapt)
{
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,21,0,99,1)
	AMDGPUInfoPtr info = AMDGPUPTR(xf86ScreenToScrn(pScreen));
	return info->glamor.xv_init(pScreen, num_adapt);
#else
	return glamor_xv_init(pScreen, num_adapt);
#endif
}

#endif /* USE_GLAMOR */
