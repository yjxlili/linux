// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Icenowy Zheng <icenowy@aosc.io>
 *
 * Based on sun4i_layer.h, which is:
 *   Copyright (C) 2015 Free Electrons
 *   Copyright (C) 2015 NextThing Co
 *
 *   Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_probe_helper.h>

#include "sun8i_mixer.h"
#include "sun8i_ui_layer.h"
#include "sun8i_ui_scaler.h"
#include "sun8i_vi_scaler.h"

static void sun8i_ui_layer_update_alpha(struct sun8i_mixer *mixer, int channel,
					int overlay, struct drm_plane *plane)
{
	u32 mask, val, ch_base;

	ch_base = sun8i_channel_base(mixer, channel);

	mask = SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA_MODE_MASK |
		SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA_MASK;

	val = SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA(plane->state->alpha >> 8);

	val |= (plane->state->alpha == DRM_BLEND_ALPHA_OPAQUE) ?
		SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA_MODE_PIXEL :
		SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA_MODE_COMBINED;

	regmap_update_bits(mixer->engine.regs,
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR(ch_base, overlay),
			   mask, val);
}

static int sun8i_ui_layer_update_coord(struct sun8i_mixer *mixer, int channel,
				       int overlay, struct drm_plane *plane,
				       unsigned int zpos)
{
	struct drm_plane_state *state = plane->state;
	u32 src_w, src_h, dst_w, dst_h;
	struct regmap *bld_regs;
	u32 bld_base, ch_base;
	u32 outsize, insize;
	u32 hphase, vphase;

	DRM_DEBUG_DRIVER("Updating UI channel %d overlay %d\n",
			 channel, overlay);

	bld_base = sun8i_blender_base(mixer);
	bld_regs = sun8i_blender_regmap(mixer);
	ch_base = sun8i_channel_base(mixer, channel);

	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;
	dst_w = drm_rect_width(&state->dst);
	dst_h = drm_rect_height(&state->dst);

	hphase = state->src.x1 & 0xffff;
	vphase = state->src.y1 & 0xffff;

	insize = SUN8I_MIXER_SIZE(src_w, src_h);
	outsize = SUN8I_MIXER_SIZE(dst_w, dst_h);

	/* Set height and width */
	DRM_DEBUG_DRIVER("Layer source offset X: %d Y: %d\n",
			 state->src.x1 >> 16, state->src.y1 >> 16);
	DRM_DEBUG_DRIVER("Layer source size W: %d H: %d\n", src_w, src_h);
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_UI_LAYER_SIZE(ch_base, overlay),
		     insize);
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_UI_OVL_SIZE(ch_base),
		     insize);

	if (insize != outsize || hphase || vphase) {
		u32 hscale, vscale;

		DRM_DEBUG_DRIVER("HW scaling is enabled\n");

		hscale = state->src_w / state->crtc_w;
		vscale = state->src_h / state->crtc_h;

		if (mixer->cfg->de_type == SUN8I_MIXER_DE33) {
			sun8i_vi_scaler_setup(mixer, channel, src_w, src_h,
					      dst_w, dst_h, hscale, vscale,
					      hphase, vphase,
					      state->fb->format);
			sun8i_vi_scaler_enable(mixer, channel, true);
		} else {
			sun8i_ui_scaler_setup(mixer, channel, src_w, src_h,
					      dst_w, dst_h, hscale, vscale,
					      hphase, vphase);
			sun8i_ui_scaler_enable(mixer, channel, true);
		}
	} else {
		DRM_DEBUG_DRIVER("HW scaling is not needed\n");
		if (mixer->cfg->de_type == SUN8I_MIXER_DE33)
			sun8i_vi_scaler_enable(mixer, channel, false);
		else
			sun8i_ui_scaler_enable(mixer, channel, false);
	}

	/* Set base coordinates */
	DRM_DEBUG_DRIVER("Layer destination coordinates X: %d Y: %d\n",
			 state->dst.x1, state->dst.y1);
	DRM_DEBUG_DRIVER("Layer destination size W: %d H: %d\n", dst_w, dst_h);
	regmap_write(bld_regs,
		     SUN8I_MIXER_BLEND_ATTR_COORD(bld_base, zpos),
		     SUN8I_MIXER_COORD(state->dst.x1, state->dst.y1));
	regmap_write(bld_regs,
		     SUN8I_MIXER_BLEND_ATTR_INSIZE(bld_base, zpos),
		     outsize);

	return 0;
}

static int sun8i_ui_layer_update_formats(struct sun8i_mixer *mixer, int channel,
					 int overlay, struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	const struct drm_format_info *fmt;
	u32 val, ch_base, hw_fmt;
	int ret;

	ch_base = sun8i_channel_base(mixer, channel);

	fmt = state->fb->format;
	ret = sun8i_mixer_drm_format_to_hw(fmt->format, &hw_fmt);
	if (ret || fmt->is_yuv) {
		DRM_DEBUG_DRIVER("Invalid format\n");
		return -EINVAL;
	}

	val = hw_fmt << SUN8I_MIXER_CHAN_UI_LAYER_ATTR_FBFMT_OFFSET;
	regmap_update_bits(mixer->engine.regs,
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR(ch_base, overlay),
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR_FBFMT_MASK, val);

	return 0;
}

static int sun8i_ui_layer_update_buffer(struct sun8i_mixer *mixer, int channel,
					int overlay, struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_dma_object *gem;
	dma_addr_t dma_addr;
	u32 ch_base;
	int bpp;

	ch_base = sun8i_channel_base(mixer, channel);

	/* Get the physical address of the buffer in memory */
	gem = drm_fb_dma_get_gem_obj(fb, 0);

	DRM_DEBUG_DRIVER("Using GEM @ %pad\n", &gem->dma_addr);

	/* Compute the start of the displayed memory */
	bpp = fb->format->cpp[0];
	dma_addr = gem->dma_addr + fb->offsets[0];

	/* Fixup framebuffer address for src coordinates */
	dma_addr += (state->src.x1 >> 16) * bpp;
	dma_addr += (state->src.y1 >> 16) * fb->pitches[0];

	/* Set the line width */
	DRM_DEBUG_DRIVER("Layer line width: %d bytes\n", fb->pitches[0]);
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_UI_LAYER_PITCH(ch_base, overlay),
		     fb->pitches[0]);

	DRM_DEBUG_DRIVER("Setting buffer address to %pad\n", &dma_addr);

	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_UI_LAYER_TOP_LADDR(ch_base, overlay),
		     lower_32_bits(dma_addr));

	return 0;
}

static int sun8i_ui_layer_atomic_check(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct sun8i_layer *layer = plane_to_sun8i_layer(plane);
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_crtc_state *crtc_state;
	int min_scale, max_scale;

	if (!crtc)
		return 0;

	crtc_state = drm_atomic_get_existing_crtc_state(state,
							crtc);
	if (WARN_ON(!crtc_state))
		return -EINVAL;

	min_scale = DRM_PLANE_NO_SCALING;
	max_scale = DRM_PLANE_NO_SCALING;

	if (layer->mixer->cfg->scaler_mask & BIT(layer->channel)) {
		min_scale = SUN8I_UI_SCALER_SCALE_MIN;
		max_scale = SUN8I_UI_SCALER_SCALE_MAX;
	}

	return drm_atomic_helper_check_plane_state(new_plane_state,
						   crtc_state,
						   min_scale, max_scale,
						   true, true);
}


static void sun8i_ui_layer_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	struct sun8i_layer *layer = plane_to_sun8i_layer(plane);
	unsigned int zpos = new_state->normalized_zpos;
	struct sun8i_mixer *mixer = layer->mixer;

	if (!new_state->crtc || !new_state->visible)
		return;

	sun8i_ui_layer_update_coord(mixer, layer->channel,
				    layer->overlay, plane, zpos);
	sun8i_ui_layer_update_alpha(mixer, layer->channel,
				    layer->overlay, plane);
	sun8i_ui_layer_update_formats(mixer, layer->channel,
				      layer->overlay, plane);
	sun8i_ui_layer_update_buffer(mixer, layer->channel,
				     layer->overlay, plane);
}

static const struct drm_plane_helper_funcs sun8i_ui_layer_helper_funcs = {
	.atomic_check	= sun8i_ui_layer_atomic_check,
	.atomic_update	= sun8i_ui_layer_atomic_update,
};

static const struct drm_plane_funcs sun8i_ui_layer_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.destroy		= drm_plane_cleanup,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.reset			= drm_atomic_helper_plane_reset,
	.update_plane		= drm_atomic_helper_update_plane,
};

static const u32 sun8i_ui_layer_formats[] = {
	DRM_FORMAT_ABGR1555,
	DRM_FORMAT_ABGR4444,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_BGRA5551,
	DRM_FORMAT_BGRA4444,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_RGBA4444,
	DRM_FORMAT_RGBA5551,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
};

static const uint64_t sun8i_layer_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

struct sun8i_layer *sun8i_ui_layer_init_one(struct drm_device *drm,
					    struct sun8i_mixer *mixer,
					    int index)
{
	enum drm_plane_type type = DRM_PLANE_TYPE_OVERLAY;
	int channel = mixer->cfg->vi_num + index;
	struct sun8i_layer *layer;
	unsigned int plane_cnt;
	int ret;

	layer = devm_kzalloc(drm->dev, sizeof(*layer), GFP_KERNEL);
	if (!layer)
		return ERR_PTR(-ENOMEM);

	if (index == 0)
		type = DRM_PLANE_TYPE_PRIMARY;

	/* possible crtcs are set later */
	ret = drm_universal_plane_init(drm, &layer->plane, 0,
				       &sun8i_ui_layer_funcs,
				       sun8i_ui_layer_formats,
				       ARRAY_SIZE(sun8i_ui_layer_formats),
				       sun8i_layer_modifiers, type, NULL);
	if (ret) {
		dev_err(drm->dev, "Couldn't initialize layer\n");
		return ERR_PTR(ret);
	}

	plane_cnt = mixer->cfg->ui_num + mixer->cfg->vi_num;

	ret = drm_plane_create_alpha_property(&layer->plane);
	if (ret) {
		dev_err(drm->dev, "Couldn't add alpha property\n");
		return ERR_PTR(ret);
	}

	ret = drm_plane_create_zpos_property(&layer->plane, channel,
					     0, plane_cnt - 1);
	if (ret) {
		dev_err(drm->dev, "Couldn't add zpos property\n");
		return ERR_PTR(ret);
	}

	drm_plane_helper_add(&layer->plane, &sun8i_ui_layer_helper_funcs);
	layer->mixer = mixer;
	layer->type = SUN8I_LAYER_TYPE_UI;
	layer->channel = channel;
	layer->overlay = 0;

	return layer;
}
