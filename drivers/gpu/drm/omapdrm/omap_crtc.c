/*
 * drivers/gpu/drm/omapdrm/omap_crtc.c
 *
 * Copyright (C) 2011 Texas Instruments
 * Author: Rob Clark <rob@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "omap_drv.h"

#include <drm/drm_mode.h>
#include "drm_crtc.h"
#include "drm_crtc_helper.h"

#define to_omap_crtc(x) container_of(x, struct omap_crtc, base)

struct omap_crtc {
	struct drm_crtc base;
	struct drm_plane *plane;

	const char *name;
	int pipe;
	enum omap_channel channel;
	struct omap_overlay_manager_info info;
	struct drm_encoder *current_encoder;

	/*
	 * Temporary: eventually this will go away, but it is needed
	 * for now to keep the output's happy.  (They only need
	 * mgr->id.)  Eventually this will be replaced w/ something
	 * more common-panel-framework-y
	 */
	struct omap_overlay_manager *mgr;

	struct omap_video_timings timings;
	bool enabled;
	bool full_update;

	/* tracks the state of GO bit between irq handler and apply worker */
	bool go_bit_set;

	struct omap_drm_apply apply;
	struct omap_drm_apply mgr_apply;

	struct omap_drm_irq apply_irq;
	struct omap_drm_irq error_irq;

	/* list of in-progress apply's: */
	struct list_head pending_applies;

	/* list of queued apply's: */
	struct list_head queued_applies;

	/* for handling queued and in-progress applies: */
	struct work_struct apply_work;

	/* if there is a pending flip, these will be non-null: */
	struct drm_pending_vblank_event *event;
	struct drm_framebuffer *old_fb;

	/* for handling page flips without caring about what
	 * the callback is called from.  Possibly we should just
	 * make omap_gem always call the cb from the worker so
	 * we don't have to care about this..
	 *
	 * XXX maybe fold into apply_work??
	 */
	struct work_struct page_flip_work;

	bool ignore_digit_sync_lost;
};

uint32_t pipe2vbl(struct drm_crtc *crtc)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);

	return dispc_mgr_get_vsync_irq(omap_crtc->channel);
}

/*
 * Manager-ops, callbacks from output when they need to configure
 * the upstream part of the video pipe.
 *
 * Most of these we can ignore until we add support for command-mode
 * panels.. for video-mode the crtc-helpers already do an adequate
 * job of sequencing the setup of the video pipe in the proper order
 */

/* ovl-mgr-id -> crtc */
static struct omap_crtc *omap_crtcs[8];

/* we can probably ignore these until we support command-mode panels: */
static int omap_crtc_connect(struct omap_overlay_manager *mgr,
		struct omap_dss_device *dst)
{
	if (mgr->output)
		return -EINVAL;

	if ((mgr->supported_outputs & dst->id) == 0)
		return -EINVAL;

	dst->manager = mgr;
	mgr->output = dst;

	return 0;
}

static void omap_crtc_disconnect(struct omap_overlay_manager *mgr,
		struct omap_dss_device *dst)
{
	mgr->output->manager = NULL;
	mgr->output = NULL;
}

static void omap_crtc_start_update(struct omap_overlay_manager *mgr)
{
}

static void set_enabled(struct drm_crtc *crtc, bool enable);

static int omap_crtc_enable(struct omap_overlay_manager *mgr)
{
	struct omap_crtc *omap_crtc = omap_crtcs[mgr->id];

	dispc_mgr_setup(omap_crtc->channel, &omap_crtc->info);
	dispc_mgr_set_timings(omap_crtc->channel,
			&omap_crtc->timings);
	set_enabled(&omap_crtc->base, true);

	return 0;
}

static void omap_crtc_disable(struct omap_overlay_manager *mgr)
{
	struct omap_crtc *omap_crtc = omap_crtcs[mgr->id];

	set_enabled(&omap_crtc->base, false);
}

static void omap_crtc_set_timings(struct omap_overlay_manager *mgr,
		const struct omap_video_timings *timings)
{
	struct omap_crtc *omap_crtc = omap_crtcs[mgr->id];
	DBG("%s", omap_crtc->name);
	omap_crtc->timings = *timings;
	omap_crtc->full_update = true;
}

static void omap_crtc_set_lcd_config(struct omap_overlay_manager *mgr,
		const struct dss_lcd_mgr_config *config)
{
	struct omap_crtc *omap_crtc = omap_crtcs[mgr->id];
	DBG("%s", omap_crtc->name);
	dispc_mgr_set_lcd_config(omap_crtc->channel, config);
}

static int omap_crtc_register_framedone_handler(
		struct omap_overlay_manager *mgr,
		void (*handler)(void *), void *data)
{
	return 0;
}

static void omap_crtc_unregister_framedone_handler(
		struct omap_overlay_manager *mgr,
		void (*handler)(void *), void *data)
{
}

static const struct dss_mgr_ops mgr_ops = {
		.connect = omap_crtc_connect,
		.disconnect = omap_crtc_disconnect,
		.start_update = omap_crtc_start_update,
		.enable = omap_crtc_enable,
		.disable = omap_crtc_disable,
		.set_timings = omap_crtc_set_timings,
		.set_lcd_config = omap_crtc_set_lcd_config,
		.register_framedone_handler = omap_crtc_register_framedone_handler,
		.unregister_framedone_handler = omap_crtc_unregister_framedone_handler,
};

/*
 * CRTC funcs:
 */

static void omap_crtc_destroy(struct drm_crtc *crtc)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);

	DBG("%s", omap_crtc->name);

	WARN_ON(omap_crtc->apply_irq.registered);
	omap_irq_unregister(crtc->dev, &omap_crtc->error_irq);

	drm_crtc_cleanup(crtc);

	kfree(omap_crtc);
}

static void omap_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct omap_drm_private *priv = crtc->dev->dev_private;
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	bool enabled = (mode == DRM_MODE_DPMS_ON);
	int i;

	DBG("%s: %d", omap_crtc->name, mode);

	if (enabled != omap_crtc->enabled) {
		omap_crtc->enabled = enabled;
		omap_crtc->full_update = true;
		omap_crtc_apply(crtc, &omap_crtc->apply);

		/* also enable our private plane: */
		WARN_ON(omap_plane_dpms(omap_crtc->plane, mode));

		/* and any attached overlay planes: */
		for (i = 0; i < priv->num_planes; i++) {
			struct drm_plane *plane = priv->planes[i];
			if (plane->crtc == crtc)
				WARN_ON(omap_plane_dpms(plane, mode));
		}
	}
}

static bool omap_crtc_mode_fixup(struct drm_crtc *crtc,
		const struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void vblank_cb(void *arg);

static void omap_crtc_cancel_page_flip(struct drm_crtc *crtc)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	struct drm_gem_object *bo;

	if (omap_crtc->old_fb == NULL)
		return;

	bo = omap_framebuffer_bo(omap_crtc->old_fb, 0);
	drm_gem_object_unreference_unlocked(bo);
	vblank_cb(crtc);
}

static int omap_crtc_mode_set(struct drm_crtc *crtc,
		struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode,
		int x, int y,
		struct drm_framebuffer *old_fb)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);

	mode = adjusted_mode;

	DBG("%s: set mode: %d:\"%s\" %d %d %d %d %d %d %d %d %d %d 0x%x 0x%x",
			omap_crtc->name, mode->base.id, mode->name,
			mode->vrefresh, mode->clock,
			mode->hdisplay, mode->hsync_start,
			mode->hsync_end, mode->htotal,
			mode->vdisplay, mode->vsync_start,
			mode->vsync_end, mode->vtotal,
			mode->type, mode->flags);

	copy_timings_drm_to_omap(&omap_crtc->timings, mode);
	omap_crtc->full_update = true;

	omap_crtc_cancel_page_flip(crtc);

	return omap_plane_mode_set(omap_crtc->plane, crtc, crtc->fb,
			0, 0, mode->hdisplay, mode->vdisplay,
			x << 16, y << 16,
			mode->hdisplay << 16, mode->vdisplay << 16,
			NULL, NULL);
}

static void omap_crtc_prepare(struct drm_crtc *crtc)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	DBG("%s", omap_crtc->name);
	omap_crtc_dpms(crtc, DRM_MODE_DPMS_OFF);
}

static void omap_crtc_commit(struct drm_crtc *crtc)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	DBG("%s", omap_crtc->name);
	omap_crtc_dpms(crtc, DRM_MODE_DPMS_ON);

	drm_modeset_unlock_all(dev);
	omap_crtc_flush(crtc);
	drm_modeset_lock_all(dev);
}

static int omap_crtc_mode_set_base(struct drm_crtc *crtc, int x, int y,
		struct drm_framebuffer *old_fb)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	struct drm_plane *plane = omap_crtc->plane;
	struct drm_display_mode *mode = &crtc->mode;

	omap_crtc_cancel_page_flip(crtc);

	return omap_plane_mode_set(plane, crtc, crtc->fb,
			0, 0, mode->hdisplay, mode->vdisplay,
			x << 16, y << 16,
			mode->hdisplay << 16, mode->vdisplay << 16,
			NULL, NULL);
}

static void vblank_cb(void *arg)
{
	struct drm_crtc *crtc = arg;
	struct drm_device *dev = crtc->dev;
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	unsigned long flags;
	struct drm_framebuffer *fb;

	spin_lock_irqsave(&dev->event_lock, flags);

	/* wakeup userspace */
	if (omap_crtc->event)
		drm_send_vblank_event(dev, omap_crtc->pipe, omap_crtc->event);

	fb = omap_crtc->old_fb;

	omap_crtc->event = NULL;
	omap_crtc->old_fb = NULL;

	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (fb)
		drm_framebuffer_unreference(fb);
}

static void page_flip_worker(struct work_struct *work)
{
	struct omap_crtc *omap_crtc =
			container_of(work, struct omap_crtc, page_flip_work);
	struct drm_crtc *crtc = &omap_crtc->base;
	struct drm_display_mode *mode = &crtc->mode;
	struct drm_gem_object *bo;

	mutex_lock(&crtc->mutex);

	/* if the page flip has been cancelled, just exit */
	if (omap_crtc->old_fb == NULL) {
		mutex_unlock(&crtc->mutex);
		return;
	}

	if (!crtc->fb) {
		/*
		 * the fb we were going to show has been removed, so cancel
		 * this page flip
		 */
		omap_crtc_cancel_page_flip(crtc);
	} else {
		omap_plane_mode_set(omap_crtc->plane, crtc, crtc->fb,
				0, 0, mode->hdisplay, mode->vdisplay,
				crtc->x << 16, crtc->y << 16,
				mode->hdisplay << 16, mode->vdisplay << 16,
				vblank_cb, crtc);

		bo = omap_framebuffer_bo(crtc->fb, 0);
		drm_gem_object_unreference_unlocked(bo);
	}

	mutex_unlock(&crtc->mutex);
}

static void page_flip_cb(void *arg)
{
	struct drm_crtc *crtc = arg;
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	struct omap_drm_private *priv = crtc->dev->dev_private;

	/* avoid assumptions about what ctxt we are called from: */
	queue_work(priv->wq, &omap_crtc->page_flip_work);
}

static int omap_crtc_page_flip_locked(struct drm_crtc *crtc,
		 struct drm_framebuffer *fb,
		 struct drm_pending_vblank_event *event,
		 uint32_t page_flip_flags)
{
	struct drm_device *dev = crtc->dev;
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	struct drm_gem_object *bo;
	unsigned long flags;

	DBG("%d -> %d (event=%p)", crtc->fb ? crtc->fb->base.id : -1,
			fb->base.id, event);

	spin_lock_irqsave(&dev->event_lock, flags);

	if (omap_crtc->old_fb) {
		spin_unlock_irqrestore(&dev->event_lock, flags);
		dev_err(dev->dev, "already a pending flip\n");
		return -EBUSY;
	}

	omap_crtc->event = event;
	omap_crtc->old_fb = crtc->fb = fb;
	drm_framebuffer_reference(omap_crtc->old_fb);

	spin_unlock_irqrestore(&dev->event_lock, flags);

	/*
	 * Hold a reference temporarily until the crtc is updated
	 * and takes the reference to the bo.  This avoids it
	 * getting freed from under us:
	 */
	bo = omap_framebuffer_bo(fb, 0);
	drm_gem_object_reference(bo);

	omap_gem_op_async(bo, OMAP_GEM_READ, page_flip_cb, crtc);

	return 0;
}

static int omap_crtc_set_property(struct drm_crtc *crtc,
		struct drm_property *property, uint64_t val)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	struct omap_drm_private *priv = crtc->dev->dev_private;
	struct omap_overlay_manager_info *info = &omap_crtc->info;
	bool mgr_property = false;

	if (property == priv->rotation_prop) {
		crtc->invert_dimensions =
				!!(val & ((1LL << DRM_ROTATE_90) | (1LL << DRM_ROTATE_270)));
	} else if (property == priv->trans_key_mode_prop) {
		mgr_property = true;

		switch (val) {
		case 0:
			info->trans_enabled = false;
			break;
		case 1:
			info->trans_enabled = true;
			info->trans_key_type = OMAP_DSS_COLOR_KEY_GFX_DST;
			break;
		case 2:
			info->trans_enabled = true;
			info->trans_key_type = OMAP_DSS_COLOR_KEY_VID_SRC;
			break;
		}
	} else if (property == priv->trans_key_prop) {
		mgr_property = true;

		info->trans_key = val;
	} else if (property == priv->background_color_prop) {
		mgr_property = true;

		info->default_color = val;
	} else if (property == priv->alpha_blender_prop) {
		mgr_property = true;

		info->partial_alpha_enabled = !!val;
	}

	if (mgr_property)
		return omap_crtc_apply(crtc, &omap_crtc->mgr_apply);
	else
		return omap_plane_set_property(omap_crtc->plane, property, val);
}

static const struct drm_crtc_funcs omap_crtc_funcs = {
	.set_config = drm_crtc_helper_set_config,
	.destroy = omap_crtc_destroy,
	.page_flip = omap_crtc_page_flip_locked,
	.set_property = omap_crtc_set_property,
};

static const struct drm_crtc_helper_funcs omap_crtc_helper_funcs = {
	.dpms = omap_crtc_dpms,
	.mode_fixup = omap_crtc_mode_fixup,
	.mode_set = omap_crtc_mode_set,
	.prepare = omap_crtc_prepare,
	.commit = omap_crtc_commit,
	.mode_set_base = omap_crtc_mode_set_base,
};

const struct omap_video_timings *omap_crtc_timings(struct drm_crtc *crtc)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	return &omap_crtc->timings;
}

enum omap_channel omap_crtc_channel(struct drm_crtc *crtc)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	return omap_crtc->channel;
}

static void omap_crtc_error_irq(struct omap_drm_irq *irq, uint32_t irqstatus)
{
	struct omap_crtc *omap_crtc =
			container_of(irq, struct omap_crtc, error_irq);

	if (omap_crtc->ignore_digit_sync_lost) {
		irqstatus &= ~DISPC_IRQ_SYNC_LOST_DIGIT;
		if (!irqstatus)
			return;
	}

	DRM_ERROR_RATELIMITED("%s: errors: %08x\n", omap_crtc->name, irqstatus);
}

static void omap_crtc_apply_irq(struct omap_drm_irq *irq, uint32_t irqstatus)
{
	struct omap_crtc *omap_crtc =
			container_of(irq, struct omap_crtc, apply_irq);
	struct drm_crtc *crtc = &omap_crtc->base;

	/* make sure we see the most recent 'go_bit_set' */
	rmb();
	if (omap_crtc->go_bit_set && !dispc_mgr_go_busy(omap_crtc->channel)) {
		struct omap_drm_private *priv =
				crtc->dev->dev_private;
		DBG("%s: apply done", omap_crtc->name);
		__omap_irq_unregister(crtc->dev, &omap_crtc->apply_irq);
		omap_crtc->go_bit_set = false;
		/* make sure apple_worker sees 'go_bit_set = false' */
		wmb();
		queue_work(priv->wq, &omap_crtc->apply_work);
	}
}

static void apply_worker(struct work_struct *work)
{
	struct omap_crtc *omap_crtc =
			container_of(work, struct omap_crtc, apply_work);
	struct drm_crtc *crtc = &omap_crtc->base;
	struct drm_device *dev = crtc->dev;
	struct omap_drm_apply *apply, *n;
	bool need_apply;

	/*
	 * Synchronize everything on mode_config.mutex, to keep
	 * the callbacks and list modification all serialized
	 * with respect to modesetting ioctls from userspace.
	 */
	mutex_lock(&crtc->mutex);
	dispc_runtime_get();

	/*
	 * If we are still pending a previous update, wait.. when the
	 * pending update completes, we get kicked again.
	 */
	/* make sure we see the most recent 'go_bit_set' */
	rmb();
	if (omap_crtc->go_bit_set)
		goto out;

	/* finish up previous apply's: */
	list_for_each_entry_safe(apply, n,
			&omap_crtc->pending_applies, pending_node) {
		apply->post_apply(apply);
		list_del(&apply->pending_node);
	}

	need_apply = !list_empty(&omap_crtc->queued_applies);

	/* then handle the next round of of queued apply's: */
	list_for_each_entry_safe(apply, n,
			&omap_crtc->queued_applies, queued_node) {
		apply->pre_apply(apply);
		list_del(&apply->queued_node);
		apply->queued = false;
		list_add_tail(&apply->pending_node,
				&omap_crtc->pending_applies);
	}

	if (need_apply) {
		enum omap_channel channel = omap_crtc->channel;

		DBG("%s: GO", omap_crtc->name);

		if (dispc_mgr_is_enabled(channel)) {
			omap_irq_register(dev, &omap_crtc->apply_irq);
			dispc_mgr_go(channel);
			omap_crtc->go_bit_set = true;
			/* make sure the irq handler sees 'go_bit_set' */
			wmb();
		} else {
			struct omap_drm_private *priv = dev->dev_private;
			queue_work(priv->wq, &omap_crtc->apply_work);
		}
	}

out:
	dispc_runtime_put();
	mutex_unlock(&crtc->mutex);
}

int omap_crtc_apply(struct drm_crtc *crtc,
		struct omap_drm_apply *apply)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);

	WARN_ON(!mutex_is_locked(&crtc->mutex));

	/* no need to queue it again if it is already queued: */
	if (apply->queued)
		return 0;

	apply->queued = true;
	list_add_tail(&apply->queued_node, &omap_crtc->queued_applies);

	/*
	 * If there are no currently pending updates, then go ahead and
	 * kick the worker immediately, otherwise it will run again when
	 * the current update finishes.
	 */
	if (list_empty(&omap_crtc->pending_applies)) {
		struct omap_drm_private *priv = crtc->dev->dev_private;
		queue_work(priv->wq, &omap_crtc->apply_work);
	}

	return 0;
}

/* called only from apply */
static void set_enabled(struct drm_crtc *crtc, bool enable)
{
	struct drm_device *dev = crtc->dev;
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	enum omap_channel channel = omap_crtc->channel;
	struct omap_irq_wait *wait;
	u32 framedone_irq, vsync_irq;
	int ret;

	if (omap_crtc->mgr->output->output_type == OMAP_DISPLAY_TYPE_HDMI) {
		dispc_mgr_enable(channel, enable);
		return;
	}

	if (dispc_mgr_is_enabled(channel) == enable)
		return;

	if (omap_crtc->channel == OMAP_DSS_CHANNEL_DIGIT) {
		/*
		 * Digit output produces some sync lost interrupts during the
		 * first frame when enabling, so we need to ignore those.
		 */
		omap_crtc->ignore_digit_sync_lost = true;
	}

	framedone_irq = dispc_mgr_get_framedone_irq(channel);
	vsync_irq = dispc_mgr_get_vsync_irq(channel);

	if (enable) {
		wait = omap_irq_wait_init(dev, vsync_irq, 1);
	} else {
		/*
		 * When we disable the digit output, we need to wait for
		 * FRAMEDONE to know that DISPC has finished with the output.
		 *
		 * OMAP2/3 does not have FRAMEDONE irq for digit output, and in
		 * that case we need to use vsync interrupt, and wait for both
		 * even and odd frames.
		 */

		if (framedone_irq)
			wait = omap_irq_wait_init(dev, framedone_irq, 1);
		else
			wait = omap_irq_wait_init(dev, vsync_irq, 2);
	}

	dispc_mgr_enable(channel, enable);

	ret = omap_irq_wait(dev, wait, msecs_to_jiffies(100));
	if (ret) {
		dev_err(dev->dev, "%s: timeout waiting for %s\n",
				omap_crtc->name, enable ? "enable" : "disable");
	}

	if (omap_crtc->channel == OMAP_DSS_CHANNEL_DIGIT) {
		omap_crtc->ignore_digit_sync_lost = false;
		/* make sure the irq handler sees the value above */
		mb();
	}
}

static void omap_crtc_mgr_pre_apply(struct omap_drm_apply *apply)
{
	struct omap_crtc *omap_crtc =
			container_of(apply, struct omap_crtc, mgr_apply);

	dispc_mgr_setup(omap_crtc->channel, &omap_crtc->info);
}

static void omap_crtc_mgr_post_apply(struct omap_drm_apply *apply)
{
	/* nothing needed for post-apply */
}

static void omap_crtc_pre_apply(struct omap_drm_apply *apply)
{
	struct omap_crtc *omap_crtc =
			container_of(apply, struct omap_crtc, apply);
	struct drm_crtc *crtc = &omap_crtc->base;
	struct drm_encoder *encoder = NULL;

	DBG("%s: enabled=%d, full=%d", omap_crtc->name,
			omap_crtc->enabled, omap_crtc->full_update);

	if (omap_crtc->full_update) {
		struct omap_drm_private *priv = crtc->dev->dev_private;
		int i;
		for (i = 0; i < priv->num_encoders; i++) {
			if (priv->encoders[i]->crtc == crtc) {
				encoder = priv->encoders[i];
				break;
			}
		}
	}

	if (omap_crtc->current_encoder && encoder != omap_crtc->current_encoder)
		omap_encoder_set_enabled(omap_crtc->current_encoder, false);

	omap_crtc->current_encoder = encoder;

	if (!omap_crtc->enabled) {
		if (encoder)
			omap_encoder_set_enabled(encoder, false);
	} else {
		if (encoder) {
			omap_encoder_set_enabled(encoder, false);
			omap_encoder_update(encoder, omap_crtc->mgr,
					&omap_crtc->timings);
			omap_encoder_set_enabled(encoder, true);
		}
	}

	omap_crtc->full_update = false;
}

static void omap_crtc_post_apply(struct omap_drm_apply *apply)
{
	/* nothing needed for post-apply */
}

static bool omap_crtc_work_pending(struct omap_crtc *omap_crtc)
{
	return !list_empty(&omap_crtc->pending_applies) ||
		!list_empty(&omap_crtc->queued_applies) ||
		omap_crtc->event || omap_crtc->old_fb;
}

/*
 * Wait for any work on the workqueue to be finished, and any work which will
 * be run via vsync irq to be done.
 *
 * Note that work on workqueue could schedule new vsync work, and vice versa.
 */
void omap_crtc_flush(struct drm_crtc *crtc)
{
	struct omap_crtc *omap_crtc = to_omap_crtc(crtc);
	struct omap_drm_private *priv = crtc->dev->dev_private;
	int loops = 0;

	while (true) {
		/* first flush the wq, so that any scheduled work is done */
		flush_workqueue(priv->wq);

		/* if we have nothing queued for this crtc, we're done */
		if (!omap_crtc_work_pending(omap_crtc))
			break;

		if (++loops > 10) {
			dev_err(crtc->dev->dev, "omap_crtc_flush() timeout\n");
			break;
		}

		/*
		 * wait for a bit so that a vsync has (probably) happened, and
		 * that the crtc work is (probably) done.
		 */
		schedule_timeout_uninterruptible(msecs_to_jiffies(20));
	}
}

static const char *channel_names[] = {
		[OMAP_DSS_CHANNEL_LCD] = "lcd",
		[OMAP_DSS_CHANNEL_DIGIT] = "tv",
		[OMAP_DSS_CHANNEL_LCD2] = "lcd2",
		[OMAP_DSS_CHANNEL_LCD3] = "lcd3",
};

void omap_crtc_pre_init(void)
{
	dss_install_mgr_ops(&mgr_ops);
}

void omap_crtc_pre_uninit(void)
{
	dss_uninstall_mgr_ops();
}

static void omap_crtc_install_properties(struct drm_crtc *crtc)
{
	struct drm_mode_object *obj = &crtc->base;
	struct drm_device *dev = crtc->dev;
	struct omap_drm_private *priv = dev->dev_private;
	struct drm_property *prop;

	prop = priv->trans_key_mode_prop;
	if (!prop) {
		static const struct drm_prop_enum_list list[] = {
			{ 0, "disable"},
			{ 1, "gfx-dst"},
			{ 2, "vid-src"},
		};
		prop = drm_property_create_enum(dev, 0, "trans-key-mode",
					 list, ARRAY_SIZE(list));
		if (prop == NULL)
			return;
		priv->trans_key_mode_prop = prop;
	}
	drm_object_attach_property(obj, prop, 0);

	prop = priv->trans_key_prop;
	if (!prop) {
		prop = drm_property_create_range(dev, 0, "trans-key",
					 0, 0xffffff);
		if (prop == NULL)
			return;
		priv->trans_key_prop = prop;
	}
	drm_object_attach_property(obj, prop, 0);

	prop = priv->background_color_prop;
	if (!prop) {
		prop = drm_property_create_range(dev, 0, "background",
					 0, 0xffffff);
		if (prop == NULL)
			return;
		priv->background_color_prop = prop;
	}
	drm_object_attach_property(obj, prop, 0);

	prop = priv->alpha_blender_prop;
	if (!prop) {
		static const struct drm_prop_enum_list list[] = {
			{ 0, "disable"},
			{ 1, "enable"},
		};
		prop = drm_property_create_enum(dev, 0, "alpha_blender",
					 list, ARRAY_SIZE(list));
		if (prop == NULL)
			return;
		priv->alpha_blender_prop = prop;
	}
	drm_object_attach_property(obj, prop, 0);
}

/* initialize crtc */
struct drm_crtc *omap_crtc_init(struct drm_device *dev,
		struct drm_plane *plane, enum omap_channel channel, int id)
{
	struct drm_crtc *crtc = NULL;
	struct omap_crtc *omap_crtc;
	struct omap_overlay_manager_info *info;

	DBG("%s", channel_names[channel]);

	omap_crtc = kzalloc(sizeof(*omap_crtc), GFP_KERNEL);
	if (!omap_crtc)
		goto fail;

	crtc = &omap_crtc->base;

	INIT_WORK(&omap_crtc->page_flip_work, page_flip_worker);
	INIT_WORK(&omap_crtc->apply_work, apply_worker);

	INIT_LIST_HEAD(&omap_crtc->pending_applies);
	INIT_LIST_HEAD(&omap_crtc->queued_applies);

	omap_crtc->apply.pre_apply  = omap_crtc_pre_apply;
	omap_crtc->apply.post_apply = omap_crtc_post_apply;

	omap_crtc->mgr_apply.pre_apply  = omap_crtc_mgr_pre_apply;
	omap_crtc->mgr_apply.post_apply = omap_crtc_mgr_post_apply;

	omap_crtc->channel = channel;
	omap_crtc->plane = plane;
	omap_crtc->plane->crtc = crtc;
	omap_crtc->name = channel_names[channel];
	omap_crtc->pipe = id;

	omap_crtc->apply_irq.irqmask = pipe2vbl(crtc);
	omap_crtc->apply_irq.irq = omap_crtc_apply_irq;

	omap_crtc->error_irq.irqmask =
			dispc_mgr_get_sync_lost_irq(channel);
	omap_crtc->error_irq.irq = omap_crtc_error_irq;
	omap_irq_register(dev, &omap_crtc->error_irq);

	/* temporary: */
	omap_crtc->mgr = omap_dss_get_overlay_manager(channel);

	/* TODO: fix hard-coded setup.. add properties! */
	info = &omap_crtc->info;
	info->default_color = 0x00000000;
	info->trans_key = 0x00000000;
	info->trans_key_type = OMAP_DSS_COLOR_KEY_GFX_DST;
	info->trans_enabled = false;

	drm_crtc_init(dev, crtc, &omap_crtc_funcs);
	drm_crtc_helper_add(crtc, &omap_crtc_helper_funcs);

	omap_crtc_install_properties(crtc);
	omap_plane_install_properties(omap_crtc->plane, &crtc->base);

	omap_crtcs[channel] = omap_crtc;

	return crtc;

fail:
	if (crtc)
		omap_crtc_destroy(crtc);

	return NULL;
}