/*
 *  linux/drivers/video/console/fbcon_ud.c -- Software Rotation - 180 degrees
 *
 *      Copyright (C) 2005 Antonino Daplas <adaplas @pol.net>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fb.h>
#include <linux/vt_kern.h>
#include <linux/console.h>
#include <asm/types.h>
#include "fbcon.h"
#include "fbcon_rotate.h"


/*
 * Rotation 180 degrees
 */

static void ud_update_attr(u8 *dst, const u8 *src, int attribute,
				  struct fbcon_display *p)
{
	int i, offset = (p->cell_height < 10) ? 1 : 2;
	int width = (p->font_width + 7) >> 3;
	unsigned int glyphsize = p->font_height * width;
	u8 c;

	offset = offset * width;

	for (i = 0; i < glyphsize; i++) {
		c = src[i];
		if (attribute & FBCON_ATTRIBUTE_UNDERLINE && i < offset)
			c = 0xff;
		if (attribute & FBCON_ATTRIBUTE_BOLD)
			c |= c << 1;
		if (attribute & FBCON_ATTRIBUTE_REVERSE)
			c = ~c;
		dst[i] = c;
	}
}


static void ud_bmove(struct vc_data *vc, struct fb_info *info, int sy,
		     int sx, int dy, int dx, int height, int width)
{
	struct fbcon_ops *ops = info->fbcon_par;
	struct fbcon_display *p = ops->p;

	struct fb_copyarea area;
	u32 vyres = GETVYRES(ops->p, info);
	u32 vxres = GETVXRES(ops->p, info);

	area.sy = vyres - ((sy + height) * p->cell_height);
	area.sx = vxres - ((sx + width) * p->cell_width);
	area.dy = vyres - ((dy + height) * p->cell_height);
	area.dx = vxres - ((dx + width) * p->cell_width);
	area.height = height * p->cell_height;
	area.width  = width * p->cell_width;

	info->fbops->fb_copyarea(info, &area);
}

static void ud_clear(struct vc_data *vc, struct fb_info *info, int sy,
		     int sx, int height, int width)
{
	struct fbcon_ops *ops = info->fbcon_par;
	struct fbcon_display *p = ops->p;

	struct fb_fillrect region;
	u32 vyres = GETVYRES(ops->p, info);
	u32 vxres = GETVXRES(ops->p, info);

	region.color = attr_bgcol_ec(vc,info);
	region.dy = vyres - ((sy + height) * p->cell_height);
	region.dx = vxres - ((sx + width) *  p->cell_width);
	region.width = width * p->cell_width;
	region.height = height * p->cell_height;
	region.rop = ROP_COPY;

	info->fbops->fb_fillrect(info, &region);
}

static inline void ud_putcs_aligned(struct vc_data *vc, struct fb_info *info,
				    const struct vc_cell *s, u32 attr, u32 cnt,
				    u32 d_pitch, u32 s_pitch, u32 glyphsize,
				    struct fb_image *image, u8 *buf, u8 *dst)
{
	struct fbcon_ops *ops = info->fbcon_par;
	struct fbcon_display *p = ops->p;

	u32 idx = p->font_width >> 3;
	u8 *src;

	while (cnt--) {
		src = ops->fontbuffer + ((s--)->glyph) * glyphsize;

		if (attr) {
			ud_update_attr(buf, src, attr, p);
			src = buf;
		}

		if (likely(idx == 1))
			__fb_pad_aligned_buffer(dst, d_pitch, src, idx,
						image->height);
		else
			fb_pad_aligned_buffer(dst, d_pitch, src, idx,
					      image->height);

		dst += s_pitch;
	}

	info->fbops->fb_imageblit(info, image);
}

static inline void ud_putcs_unaligned(struct vc_data *vc,
				      struct fb_info *info,
				      const struct vc_cell *s,
				      u32 attr, u32 cnt, u32 d_pitch,
				      u32 s_pitch, u32 glyphsize,
				      struct fb_image *image, u8 *buf,
				      u8 *dst)
{
	struct fbcon_ops *ops = info->fbcon_par;
	struct fbcon_display *p = ops->p;

	u32 shift_low = 0, mod = p->font_width % 8;
	u32 shift_high = 8;
	u32 idx = p->font_width >> 3;
	u8 *src;

	while (cnt--) {
		src = ops->fontbuffer + ((s--)->glyph) * glyphsize;

		if (attr) {
			ud_update_attr(buf, src, attr, p);
			src = buf;
		}

		fb_pad_unaligned_buffer(dst, d_pitch, src, idx,
					image->height, shift_high,
					shift_low, mod);
		shift_low += mod;
		dst += (shift_low >= 8) ? s_pitch : s_pitch - 1;
		shift_low &= 7;
		shift_high = 8 - shift_low;
	}

	info->fbops->fb_imageblit(info, image);

}

static void ud_putcs(struct vc_data *vc, struct fb_info *info,
		      const struct vc_cell *s, int count, int yy, int xx,
		      int fg, int bg)
{
	struct fb_image image;
	struct fbcon_ops *ops = info->fbcon_par;
	struct fbcon_display *p = ops->p;

	u32 width = (p->font_width + 7) / 8;
	u32 glyphsize = width * p->font_height;
	u32 maxcnt = info->pixmap.size / glyphsize;
	u32 scan_align = info->pixmap.scan_align - 1;
	u32 buf_align = info->pixmap.buf_align - 1;
	u32 mod = p->font_width % 8, cnt, pitch, size;
	u32 attribute = get_attribute(info, *s);
	u8 *dst, *buf = NULL;
	u32 vyres = GETVYRES(ops->p, info);
	u32 vxres = GETVXRES(ops->p, info);

	if (!ops->fontbuffer)
		return;

	image.fg_color = fg;
	image.bg_color = bg;
	image.dy = vyres - ((yy * p->cell_height) + p->cell_height);
	image.dx = vxres - ((xx + count) * p->cell_width) +
			(p->cell_width - p->font_width);
	image.height = p->font_height;
	image.depth = 1;

	if (attribute) {
		buf = kmalloc(glyphsize, GFP_KERNEL);
		if (!buf)
			return;
	}

	s += count - 1;

	while (count) {
		if (p->font_width != p->cell_width)
			cnt = 1;
		else if (count > maxcnt)
			cnt = maxcnt;
		else
			cnt = count;

		image.width = p->font_width * cnt;
		pitch = ((image.width + 7) >> 3) + scan_align;
		pitch &= ~scan_align;
		size = pitch * image.height + buf_align;
		size &= ~buf_align;
		dst = fb_get_buffer_offset(info, &info->pixmap, size);
		image.data = dst;

		if (!mod)
			ud_putcs_aligned(vc, info, s, attribute, cnt, pitch,
					 width, glyphsize, &image, buf, dst);
		else
			ud_putcs_unaligned(vc, info, s, attribute, cnt, pitch,
					   width, glyphsize, &image,
					   buf, dst);

		image.dx += cnt * p->cell_width;
		count -= cnt;
		s -= cnt;
	}

	/* buf is always NULL except when in monochrome mode, so in this case
	   it's a gain to check buf against NULL even though kfree() handles
	   NULL pointers just fine */
	if (unlikely(buf))
		kfree(buf);

}

static void ud_clear_margins(struct vc_data *vc, struct fb_info *info,
			     int color, int bottom_only)
{
	struct fbcon_ops *ops = info->fbcon_par;
	struct fbcon_display *p = ops->p;

	unsigned int cw = p->cell_width;
	unsigned int ch = p->cell_height;
	unsigned int rw = info->var.xres - (vc->vc_cols*cw);
	unsigned int bh = info->var.yres - (vc->vc_rows*ch);
	struct fb_fillrect region;

	region.color = color;
	region.rop = ROP_COPY;

	if ((int) rw > 0 && !bottom_only) {
		region.dy = 0;
		region.dx = info->var.xoffset;
		region.width  = rw;
		region.height = info->var.yres_virtual;
		info->fbops->fb_fillrect(info, &region);
	}

	if ((int) bh > 0) {
		region.dy = info->var.yoffset;
		region.dx = info->var.xoffset;
                region.height  = bh;
                region.width = info->var.xres;
		info->fbops->fb_fillrect(info, &region);
	}
}

static void ud_cursor(struct vc_data *vc, struct fb_info *info, int mode,
		      int fg, int bg)
{
	struct fbcon_ops *ops = info->fbcon_par;
	struct fbcon_display *p = ops->p;

	struct fb_cursor cursor;
	int w;
	struct vc_cell c;
	int y = real_y(ops->p, vc->state.y);
	int attribute, use_sw = vc->vc_cursor_type & CUR_SW;
	int err = 1, dx, dy, pad;
	char *src;
	u32 vyres = GETVYRES(ops->p, info);
	u32 vxres = GETVXRES(ops->p, info);

	if (!ops->fontbuffer)
		return;

	cursor.set = 0;

	c = *vc->vc_pos;
	attribute = get_attribute(info, c);
	w = (p->font_width + 7) >> 3;
	src = ops->fontbuffer + (c.glyph * (w * p->font_height));

	if (ops->cursor_state.image.data != src ||
	    ops->cursor_reset) {
	    ops->cursor_state.image.data = src;
	    cursor.set |= FB_CUR_SETIMAGE;
	}

	if (attribute) {
		u8 *dst;

		dst = kmalloc_array(w, p->font_height, GFP_ATOMIC);
		if (!dst)
			return;
		kfree(ops->cursor_data);
		ops->cursor_data = dst;
		ud_update_attr(dst, src, attribute, p);
		src = dst;
	}

	if (ops->cursor_state.image.fg_color != fg ||
	    ops->cursor_state.image.bg_color != bg ||
	    ops->cursor_reset) {
		ops->cursor_state.image.fg_color = fg;
		ops->cursor_state.image.bg_color = bg;
		cursor.set |= FB_CUR_SETCMAP;
	}

	if (ops->cursor_state.image.height != p->font_height ||
	    ops->cursor_state.image.width != p->font_width ||
	    ops->cursor_reset) {
		ops->cursor_state.image.height = p->font_height;
		ops->cursor_state.image.width = p->font_width;
		cursor.set |= FB_CUR_SETSIZE;
	}

	pad = p->cell_width - p->font_width;
	dy = vyres - ((y * p->cell_height) + p->cell_height);
	dx = vxres - ((vc->state.x * p->cell_width) + p->cell_width) + pad;

	if (ops->cursor_state.image.dx != dx ||
	    ops->cursor_state.image.dy != dy ||
	    ops->cursor_reset) {
		ops->cursor_state.image.dx = dx;
		ops->cursor_state.image.dy = dy;
		cursor.set |= FB_CUR_SETPOS;
	}

	if (ops->cursor_state.hot.x || ops->cursor_state.hot.y ||
	    ops->cursor_reset) {
		ops->cursor_state.hot.x = cursor.hot.y = 0;
		cursor.set |= FB_CUR_SETHOT;
	}

	if (cursor.set & FB_CUR_SETSIZE ||
	    vc->vc_cursor_type != ops->p->cursor_shape ||
	    ops->cursor_state.mask == NULL ||
	    ops->cursor_reset) {
		char *mask = kmalloc_array(w, p->font_height, GFP_ATOMIC);
		int cur_height, size, i = 0;
		u8 msk = 0xff;

		if (!mask)
			return;

		kfree(ops->cursor_state.mask);
		ops->cursor_state.mask = mask;

		ops->p->cursor_shape = vc->vc_cursor_type;
		cursor.set |= FB_CUR_SETSHAPE;

		switch (CUR_SIZE(ops->p->cursor_shape)) {
		case CUR_NONE:
			cur_height = 0;
			break;
		case CUR_UNDERLINE:
			cur_height = (p->cell_height < 10) ? 1 : 2;
			break;
		case CUR_LOWER_THIRD:
			cur_height = p->cell_height / 3;
			break;
		case CUR_LOWER_HALF:
			cur_height = p->cell_height >> 1;
			break;
		case CUR_TWO_THIRDS:
			cur_height = (p->cell_height << 1) / 3;
			break;
		case CUR_BLOCK:
		default:
			cur_height = p->cell_height;
			break;
		}

		if (cur_height > p->font_height)
			cur_height = p->font_height;

		size = cur_height * w;

		while (size--)
			mask[i++] = msk;

		size = (p->font_height - cur_height) * w;

		while (size--)
			mask[i++] = ~msk;
	}

	switch (mode) {
	case CM_ERASE:
		ops->cursor_state.enable = 0;
		break;
	case CM_DRAW:
	case CM_MOVE:
	default:
		ops->cursor_state.enable = (use_sw) ? 0 : 1;
		break;
	}

	cursor.image.data = src;
	cursor.image.fg_color = ops->cursor_state.image.fg_color;
	cursor.image.bg_color = ops->cursor_state.image.bg_color;
	cursor.image.dx = ops->cursor_state.image.dx;
	cursor.image.dy = ops->cursor_state.image.dy;
	cursor.image.height = ops->cursor_state.image.height;
	cursor.image.width = ops->cursor_state.image.width;
	cursor.hot.x = ops->cursor_state.hot.x;
	cursor.hot.y = ops->cursor_state.hot.y;
	cursor.mask = ops->cursor_state.mask;
	cursor.enable = ops->cursor_state.enable;
	cursor.image.depth = 1;
	cursor.rop = ROP_XOR;

	if (info->fbops->fb_cursor)
		err = info->fbops->fb_cursor(info, &cursor);

	if (err)
		soft_cursor(info, &cursor);

	ops->cursor_reset = 0;
}

static int ud_update_start(struct fb_info *info)
{
	struct fbcon_ops *ops = info->fbcon_par;
	int xoffset, yoffset;
	u32 vyres = GETVYRES(ops->p, info);
	u32 vxres = GETVXRES(ops->p, info);
	int err;

	xoffset = vxres - info->var.xres - ops->var.xoffset;
	yoffset = vyres - info->var.yres - ops->var.yoffset;
	if (yoffset < 0)
		yoffset += vyres;
	ops->var.xoffset = xoffset;
	ops->var.yoffset = yoffset;
	err = fb_pan_display(info, &ops->var);
	ops->var.xoffset = info->var.xoffset;
	ops->var.yoffset = info->var.yoffset;
	ops->var.vmode = info->var.vmode;
	return err;
}

void fbcon_rotate_ud(struct fbcon_ops *ops)
{
	ops->bmove = ud_bmove;
	ops->clear = ud_clear;
	ops->putcs = ud_putcs;
	ops->clear_margins = ud_clear_margins;
	ops->cursor = ud_cursor;
	ops->update_start = ud_update_start;
}
