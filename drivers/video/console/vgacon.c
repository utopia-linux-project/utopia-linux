/*
 *  linux/drivers/video/vgacon.c -- Low level VGA based console driver
 *
 *	Created 28 Sep 1997 by Geert Uytterhoeven
 *
 *	Rewritten by Martin Mares <mj@ucw.cz>, July 1998
 *
 *  This file is based on the old console.c, vga.c and vesa_blank.c drivers.
 *
 *	Copyright (C) 1991, 1992  Linus Torvalds
 *			    1995  Jay Estabrook
 *
 *	User definable mapping table and font loading by Eugene G. Crosser,
 *	<crosser@average.org>
 *
 *	Improved loadable font/UTF-8 support by H. Peter Anvin
 *	Feb-Sep 1995 <peter.anvin@linux.org>
 *
 *	Colour palette handling, by Simon Tatham
 *	17-Jun-95 <sgt20@cam.ac.uk>
 *
 *	if 512 char mode is already enabled don't re-enable it,
 *	because it causes screen to flicker, by Mitja Horvat
 *	5-May-96 <mitja.horvat@guest.arnes.si>
 *
 *	Use 2 outw instead of 4 outb_p to reduce erroneous text
 *	flashing on RHS of screen during heavy console scrolling .
 *	Oct 1996, Paul Gortmaker.
 *
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/string.h>
#include <linux/kd.h>
#include <linux/slab.h>
#include <linux/vt_kern.h>
#include <linux/sched.h>
#include <linux/selection.h>
#include <linux/spinlock.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/screen_info.h>
#include <video/vga.h>
#include <asm/io.h>

static DEFINE_RAW_SPINLOCK(vga_lock);
static int cursor_size_lastfrom;
static int cursor_size_lastto;
static u32 vgacon_xres;
static u32 vgacon_yres;
static struct vgastate vgastate;

static const u16 BLANK = 0x0020;

#define VGA_FONTWIDTH       8   /* VGA does not support fontwidths != 8 */
/*
 *  Interface used by the world
 */

static const char *vgacon_startup(void);
static void vgacon_init(struct vc_data *c, int init);
static void vgacon_deinit(struct vc_data *c);
static void vgacon_cursor(struct vc_data *c, int mode);
static int vgacon_switch(struct vc_data *c);
static int vgacon_blank(struct vc_data *c, int blank, int mode_switch);
static void vgacon_scrollback(struct vc_data *c, int lines);
static int vgacon_reset_origin(struct vc_data *c);
static void vgacon_invert_selection(struct vc_data *c, int offset, int count);
static void vgacon_clear(struct vc_data *vc, int sy, int sx, int height, int width);
static void vgacon_putc(struct vc_data *vc, struct vc_cell c, int ypos, int xpos);
static void vgacon_putcs(struct vc_data *vc, const struct vc_cell *s, int count,
		int ypos, int xpos);

/* Description of the hardware situation */
static u16		*vga_vram_base		__read_mostly;	/* Base of video memory */
static u16		*vga_vram_end		__read_mostly;	/* End of video memory */
static unsigned int	vga_vram_size		__read_mostly;	/* Count of video memory cells */
static u16		vga_video_port_reg	__read_mostly;	/* Video register select port */
static u16		vga_video_port_val	__read_mostly;	/* Video register value port */
static unsigned int	vga_video_num_columns;			/* Number of text columns */
static unsigned int	vga_video_num_lines;			/* Number of text lines */
static bool		vga_can_do_color	__read_mostly;	/* Do we support colors? */
static unsigned int	vga_default_font_height __read_mostly;	/* Height of default screen font */
static unsigned char	vga_video_type		__read_mostly;	/* Card type */
static int		vga_vesa_blanked;
static bool 		vga_palette_blanked;
static bool 		vga_is_gfx;
static int 		vga_video_font_height;
static int 		vga_scan_lines		__read_mostly;
static u16		*vga_origin;		/* for scrolling */
static u16		*vga_visible_origin;	/* for scrollback */
static unsigned int 	vga_rolled_over;	/* offset of last vga_origin before wrap */

static bool vga_hardscroll_enabled;
static bool vga_hardscroll_user_enable = true;

static int __init no_scroll(char *str)
{
	/*
	 * Disabling scrollback is required for the Braillex ib80-piezo
	 * Braille reader made by F.H. Papenmeier (Germany).
	 * Use the "no-scroll" bootflag.
	 */
	vga_hardscroll_user_enable = vga_hardscroll_enabled = false;
	return 1;
}

__setup("no-scroll", no_scroll);

static inline void cellmove(struct vc_cell *dest, const struct vc_cell *src, size_t count)
{
	memmove(dest, src, count * sizeof(struct vc_cell));
}

static inline void cellset(struct vc_cell *dest, struct vc_cell cell, size_t count)
{
	while (count--) {
		*dest++ = cell;
	}
}

static inline u16 scr_read(const volatile u16 *p)
{
	return *p;
}

static inline void scr_write(u16 celldata, volatile u16 *p)
{
	*p = celldata;
}

static inline void scr_memset(volatile u16 *p, u16 a, size_t count)
{
	while (count--)
		*p++ = a;
}

static inline void scr_memmove(u16 *dst, const u16 *src, size_t count)
{
	memmove(dst, src, count * sizeof(u16));
}

/*
 * By replacing the four outb_p with two back to back outw, we can reduce
 * the window of opportunity to see text mislocated to the RHS of the
 * console during heavy scrolling activity. However there is the remote
 * possibility that some pre-dinosaur hardware won't like the back to back
 * I/O. Since the Xservers get away with it, we should be able to as well.
 */
static inline void write_vga(unsigned char reg, unsigned int val)
{
	unsigned int v1, v2;
	unsigned long flags;

	/*
	 * ddprintk might set the console position from interrupt
	 * handlers, thus the write has to be IRQ-atomic.
	 */
	raw_spin_lock_irqsave(&vga_lock, flags);
	v1 = reg + (val & 0xff00);
	v2 = reg + 1 + ((val << 8) & 0xff00);
	outw(v1, vga_video_port_reg);
	outw(v2, vga_video_port_reg);
	raw_spin_unlock_irqrestore(&vga_lock, flags);
}

static inline void vga_update_mem_top(struct vc_data *c)
{
	write_vga(12, vga_visible_origin - vga_vram_base);
}

static void vgacon_restore_screen(struct vc_data *c)
{
	vga_visible_origin = vga_origin;
	vga_update_mem_top(c);
}

static void vgacon_scrollback(struct vc_data *c, int lines)
{
	ptrdiff_t scr_end = (vga_origin + c->vc_screen_size) - vga_vram_base;
	ptrdiff_t vorigin = vga_visible_origin - vga_vram_base;
	ptrdiff_t origin = vga_origin - vga_vram_base;
	int margin = c->vc_cols * 4;
	int from, wrap, from_off, avail;

	/* Do we have already enough to allow jumping from 0 to the end? */
	if (vga_rolled_over > scr_end + margin) {
		from = scr_end;
		wrap = vga_rolled_over + c->vc_cols;
	} else {
		from = 0;
		wrap = vga_vram_size;
	}

	from_off = (vorigin - from + wrap) % wrap + lines * c->vc_cols;
	avail = (origin - from + wrap) % wrap;

	/* Only a little piece would be left? Show all incl. the piece! */
	if (avail < 2 * margin)
		margin = 0;
	if (from_off < margin)
		from_off = 0;
	if (from_off > avail - margin)
		from_off = avail;

	vga_visible_origin = vga_vram_base + (from + from_off) % wrap;
	vga_update_mem_top(c);
}

static const char *vgacon_startup(void)
{
	const char *display_desc = NULL;
	u16 saved1, saved2;
	volatile u16 *p;

	if (screen_info.orig_video_isVGA == VIDEO_TYPE_VLFB ||
	    screen_info.orig_video_isVGA == VIDEO_TYPE_EFI) {
	      no_vga:
#ifdef CONFIG_DUMMY_CONSOLE
		conswitchp = &dummy_con;
		return conswitchp->con_startup();
#else
		return NULL;
#endif
	}

	/* boot_params.screen_info reasonably initialized? */
	if ((screen_info.orig_video_lines == 0) ||
	    (screen_info.orig_video_cols  == 0))
		goto no_vga;

	/* VGA16 modes are not handled by VGACON */
	if ((screen_info.orig_video_mode == 0x0D) ||	/* 320x200/4 */
	    (screen_info.orig_video_mode == 0x0E) ||	/* 640x200/4 */
	    (screen_info.orig_video_mode == 0x10) ||	/* 640x350/4 */
	    (screen_info.orig_video_mode == 0x12) ||	/* 640x480/4 */
	    (screen_info.orig_video_mode == 0x6A))	/* 800x600/4 (VESA) */
		goto no_vga;

	vga_video_num_lines = screen_info.orig_video_lines;
	vga_video_num_columns = screen_info.orig_video_cols;
	vgastate.vgabase = NULL;

	if (screen_info.orig_video_mode == 7) {
		/* Monochrome display */
		vga_vram_base = (void *)0xb0000;
		vga_video_port_reg = VGA_CRT_IM;
		vga_video_port_val = VGA_CRT_DM;
		if ((screen_info.orig_video_ega_bx & 0xff) != 0x10) {
			static struct resource ega_console_resource =
			    { .name	= "ega",
			      .flags	= IORESOURCE_IO,
			      .start	= 0x3B0,
			      .end	= 0x3BF };
			vga_video_type = VIDEO_TYPE_EGAM;
			vga_vram_size = 0x8000 >> 1;
			display_desc = "EGA+";
			request_resource(&ioport_resource,
					 &ega_console_resource);
		} else {
			static struct resource mda1_console_resource =
			    { .name	= "mda",
			      .flags	= IORESOURCE_IO,
			      .start	= 0x3B0,
			      .end	= 0x3BB };
			static struct resource mda2_console_resource =
			    { .name	= "mda",
			      .flags	= IORESOURCE_IO,
			      .start	= 0x3BF,
			      .end	= 0x3BF };
			vga_video_type = VIDEO_TYPE_MDA;
			vga_vram_size = 0x2000 >> 1;
			display_desc = "*MDA";
			request_resource(&ioport_resource,
					 &mda1_console_resource);
			request_resource(&ioport_resource,
					 &mda2_console_resource);
			vga_video_font_height = 14;
		}
	} else {
		/* If not, it is color. */
		vga_can_do_color = true;
		vga_vram_base = (void *)0xb8000;
		vga_video_port_reg = VGA_CRT_IC;
		vga_video_port_val = VGA_CRT_DC;
		if ((screen_info.orig_video_ega_bx & 0xff) != 0x10) {
			int i;

			vga_vram_size = 0x8000 >> 1;

			if (!screen_info.orig_video_isVGA) {
				static struct resource ega_console_resource =
				    { .name	= "ega",
				      .flags	= IORESOURCE_IO,
				      .start	= 0x3C0,
				      .end	= 0x3DF };
				vga_video_type = VIDEO_TYPE_EGAC;
				display_desc = "EGA";
				request_resource(&ioport_resource,
						 &ega_console_resource);
			} else {
				static struct resource vga_console_resource =
				    { .name	= "vga+",
				      .flags	= IORESOURCE_IO,
				      .start	= 0x3C0,
				      .end	= 0x3DF };
				vga_video_type = VIDEO_TYPE_VGAC;
				display_desc = "VGA+";
				request_resource(&ioport_resource,
						 &vga_console_resource);

				/*
				 * Normalise the palette registers, to point
				 * the 16 screen colours to the first 16
				 * DAC entries.
				 */

				for (i = 0; i < 16; i++) {
					inb_p(VGA_IS1_RC);
					outb_p(i, VGA_ATT_W);
					outb_p(i, VGA_ATT_W);
				}
				outb_p(0x20, VGA_ATT_W);

				/*
				 * Now set the DAC registers back to their
				 * default values
				 */
				for (i = 0; i < 16; i++) {
					outb_p(color_table[i], VGA_PEL_IW);
					outb_p(default_red[i], VGA_PEL_D);
					outb_p(default_grn[i], VGA_PEL_D);
					outb_p(default_blu[i], VGA_PEL_D);
				}
			}
		} else {
			static struct resource cga_console_resource =
			    { .name	= "cga",
			      .flags	= IORESOURCE_IO,
			      .start	= 0x3D4,
			      .end	= 0x3D5 };
			vga_video_type = VIDEO_TYPE_CGA;
			vga_vram_size = 0x2000 >> 1;
			display_desc = "*CGA";
			request_resource(&ioport_resource,
					 &cga_console_resource);
			vga_video_font_height = 8;
		}
	}

	vga_vram_base = (void *)VGA_MAP_MEM((int)vga_vram_base, vga_vram_size << 1);
	vga_vram_end = vga_vram_base + vga_vram_size;

	/*
	 *      Find out if there is a graphics card present.
	 *      Are there smarter methods around?
	 */
	p = vga_vram_base;
	saved1 = scr_read(p);
	saved2 = scr_read(p + 1);
	scr_write(0xAA55, p);
	scr_write(0x55AA, p + 1);
	if (scr_read(p) != 0xAA55 || scr_read(p + 1) != 0x55AA) {
		scr_write(saved1, p);
		scr_write(saved2, p + 1);
		goto no_vga;
	}
	scr_write(0x55AA, p);
	scr_write(0xAA55, p + 1);
	if (scr_read(p) != 0x55AA || scr_read(p + 1) != 0xAA55) {
		scr_write(saved1, p);
		scr_write(saved2, p + 1);
		goto no_vga;
	}
	scr_write(saved1, p);
	scr_write(saved2, p + 1);

	if (vga_video_type == VIDEO_TYPE_EGAC
	    || vga_video_type == VIDEO_TYPE_VGAC
	    || vga_video_type == VIDEO_TYPE_EGAM) {
		vga_hardscroll_enabled = vga_hardscroll_user_enable;
		vga_default_font_height = screen_info.orig_video_points;
		vga_video_font_height = screen_info.orig_video_points;
		/* This may be suboptimal but is a safe bet - go with it */
		vga_scan_lines =
		    vga_video_font_height * vga_video_num_lines;
	}

	vgacon_xres = screen_info.orig_video_cols * VGA_FONTWIDTH;
	vgacon_yres = vga_scan_lines;

	return display_desc;
}

static void vgacon_init(struct vc_data *c, int init)
{
	/*
	 * We cannot be loaded as a module, therefore init will be 1
	 * if we are the default console, however if we are a fallback
	 * console, for example if fbcon has failed registration, then
	 * init will be 0, so we need to make sure our boot parameters
	 * have been copied to the console structure for vgacon_resize
	 * ultimately called by vc_resize.  Any subsequent calls to
	 * vgacon_init init will have init set to 0 too.
	 */
	c->vc_can_do_color = vga_can_do_color;
	c->vc_scan_lines = vga_scan_lines;

	/* set dimensions manually if init != 0 since vc_resize() will fail */
	if (init) {
		c->vc_cols = vga_video_num_columns;
		c->vc_rows = vga_video_num_lines;
	} else
		vc_resize(c, vga_video_num_columns, vga_video_num_lines);

	/* Only set the default if the user didn't deliberately override it */
	if (global_cursor_default == -1)
		global_cursor_default =
			!(screen_info.flags & VIDEO_FLAGS_NOCURSOR);
}

static void vgacon_deinit(struct vc_data *c)
{
	/* When closing the active console, reset video origin */
	if (con_is_visible(c)) {
		vga_visible_origin = vga_origin = vga_vram_base;
		vga_update_mem_top(c);
	}
}

static u8 vgacon_build_attr(struct vc_data *c, struct vc_cell_attr a)
{
	u8 attr = 0;

	bool invert = a.reverse ^ a.selected;

	if (vga_can_do_color) {
		/* CGA / EGA / VGA */
		attr = a.fg_color | (a.bg_color << 4);

		if (a.italic)
			attr = (attr & 0xf0) | c->vc_itcolor;
		else if (a.underline)
			attr = (attr & 0xf0) | c->vc_ulcolor;
		else if (a.intensity == VCI_HALF_BRIGHT)
			attr = (attr & 0xf0) | c->vc_halfcolor;

		if (a.pointer_pos) {
			attr ^= 0x77;
		}

		if (invert)
			attr =
			    ((attr) & 0x88) | ((((attr) >> 4) | ((attr) << 4)) &
					       0x77);
		if (a.blink)
			attr |= 0x80;
		if (a.intensity == VCI_BOLD)
			attr |= 0x08;
	} else {
		/*
		 * MDA. Note that MDA's reverse mode cannot be combined with
		 * underline. If you set them both, underline takes precedence.
		 */
		attr = (invert) ? 0x70 :			/* reverse */
			(a.italic || a.underline) ? 0x01 :	/* underline */
			0x07;					/* normal */

		if (a.blink || a.pointer_pos)
			attr |= 0x80;	/* blink */
		if (a.intensity == VCI_BOLD)
			attr |= 0x08;	/* bright */
	}
	return attr;
}

static u16 to_u16(struct vc_data *vc, struct vc_cell c)
{
	return (u16) vgacon_build_attr(vc, c.attr) << 8 | c.glyph;
}

/* Used by selection. Hence it uses the visible region. */
static void vgacon_invert_selection(struct vc_data *c, int offset, int count)
{
	const bool col = vga_can_do_color;
	u16 *p = vga_visible_origin + offset;

	while (count--) {
		u16 a = scr_read(p);
		if (col)
			a = ((a) & 0x88ff) | (((a) & 0x7000) >> 4) |
			    (((a) & 0x0700) << 4);
		else
			a ^= ((a & 0x0700) == 0x0100) ? 0x7000 : 0x7700;
		scr_write(a, p++);
	}
}

static void vgacon_set_cursor_size(int from, int to)
{
	unsigned long flags;
	int curs, cure;

	if ((from == cursor_size_lastfrom) && (to == cursor_size_lastto))
		return;
	cursor_size_lastfrom = from;
	cursor_size_lastto = to;

	raw_spin_lock_irqsave(&vga_lock, flags);
	if (vga_video_type >= VIDEO_TYPE_VGAC) {
		outb_p(VGA_CRTC_CURSOR_START, vga_video_port_reg);
		curs = inb_p(vga_video_port_val);
		outb_p(VGA_CRTC_CURSOR_END, vga_video_port_reg);
		cure = inb_p(vga_video_port_val);
	} else {
		curs = 0;
		cure = 0;
	}

	curs = (curs & 0xc0) | from;
	cure = (cure & 0xe0) | to;

	outb_p(VGA_CRTC_CURSOR_START, vga_video_port_reg);
	outb_p(curs, vga_video_port_val);
	outb_p(VGA_CRTC_CURSOR_END, vga_video_port_reg);
	outb_p(cure, vga_video_port_val);
	raw_spin_unlock_irqrestore(&vga_lock, flags);
}

static void vgacon_cursor(struct vc_data *c, int mode)
{
	if (c->vc_mode != KD_TEXT)
		return;

	vgacon_restore_screen(c);

	switch (mode) {
	case CM_ERASE:
		write_vga(14, (vga_origin - vga_vram_base) +
				(c->vc_pos - c->vc_screenbuf));
	        if (vga_video_type >= VIDEO_TYPE_VGAC)
			vgacon_set_cursor_size(31, 30);
		else
			vgacon_set_cursor_size(31, 31);
		break;

	case CM_MOVE:
	case CM_DRAW:
		write_vga(14, (vga_origin - vga_vram_base) +
				(c->vc_pos - c->vc_screenbuf));
		switch (CUR_SIZE(c->vc_cursor_type)) {
		case CUR_UNDERLINE:
			vgacon_set_cursor_size(vga_video_font_height -
					       (vga_video_font_height <
						10 ? 2 : 3),
					       vga_video_font_height -
					       (vga_video_font_height <
						10 ? 1 : 2));
			break;
		case CUR_TWO_THIRDS:
			vgacon_set_cursor_size(vga_video_font_height / 3,
					       vga_video_font_height -
					       (vga_video_font_height <
						10 ? 1 : 2));
			break;
		case CUR_LOWER_THIRD:
			vgacon_set_cursor_size((vga_video_font_height * 2) / 3,
					       vga_video_font_height -
					       (vga_video_font_height <
						10 ? 1 : 2));
			break;
		case CUR_LOWER_HALF:
			vgacon_set_cursor_size(vga_video_font_height / 2,
					       vga_video_font_height -
					       (vga_video_font_height <
						10 ? 1 : 2));
			break;
		case CUR_NONE:
			if (vga_video_type >= VIDEO_TYPE_VGAC)
				vgacon_set_cursor_size(31, 30);
			else
				vgacon_set_cursor_size(31, 31);
			break;
		default:
			vgacon_set_cursor_size(1, vga_video_font_height);
			break;
		}
		break;
	}
}

static int vgacon_doresize(struct vc_data *c,
		unsigned int width, unsigned int height)
{
	unsigned long flags;
	unsigned int scanlines = height * vga_video_font_height;
	u8 scanlines_lo = 0, r7 = 0, vsync_end = 0, mode, max_scan;

	raw_spin_lock_irqsave(&vga_lock, flags);

	vgacon_xres = width * VGA_FONTWIDTH;
	vgacon_yres = height * vga_video_font_height;
	if (vga_video_type >= VIDEO_TYPE_VGAC) {
		outb_p(VGA_CRTC_MAX_SCAN, vga_video_port_reg);
		max_scan = inb_p(vga_video_port_val);

		if (max_scan & 0x80)
			scanlines <<= 1;

		outb_p(VGA_CRTC_MODE, vga_video_port_reg);
		mode = inb_p(vga_video_port_val);

		if (mode & 0x04)
			scanlines >>= 1;

		scanlines -= 1;
		scanlines_lo = scanlines & 0xff;

		outb_p(VGA_CRTC_OVERFLOW, vga_video_port_reg);
		r7 = inb_p(vga_video_port_val) & ~0x42;

		if (scanlines & 0x100)
			r7 |= 0x02;
		if (scanlines & 0x200)
			r7 |= 0x40;

		/* deprotect registers */
		outb_p(VGA_CRTC_V_SYNC_END, vga_video_port_reg);
		vsync_end = inb_p(vga_video_port_val);
		outb_p(VGA_CRTC_V_SYNC_END, vga_video_port_reg);
		outb_p(vsync_end & ~0x80, vga_video_port_val);
	}

	outb_p(VGA_CRTC_H_DISP, vga_video_port_reg);
	outb_p(width - 1, vga_video_port_val);
	outb_p(VGA_CRTC_OFFSET, vga_video_port_reg);
	outb_p(width >> 1, vga_video_port_val);

	if (vga_video_type >= VIDEO_TYPE_VGAC) {
		outb_p(VGA_CRTC_V_DISP_END, vga_video_port_reg);
		outb_p(scanlines_lo, vga_video_port_val);
		outb_p(VGA_CRTC_OVERFLOW, vga_video_port_reg);
		outb_p(r7,vga_video_port_val);

		/* reprotect registers */
		outb_p(VGA_CRTC_V_SYNC_END, vga_video_port_reg);
		outb_p(vsync_end, vga_video_port_val);
	}

	raw_spin_unlock_irqrestore(&vga_lock, flags);
	return 0;
}

static int vgacon_switch(struct vc_data *c)
{
	int x = c->vc_cols * VGA_FONTWIDTH;
	int y = c->vc_rows * vga_video_font_height;
	int rows = screen_info.orig_video_lines * vga_default_font_height/
		vga_video_font_height;
	/*
	 * We need to save screen size here as it's the only way
	 * we can spot the screen has been resized and we need to
	 * set size of freshly allocated screens ourselves.
	 */
	vga_video_num_columns = c->vc_cols;
	vga_video_num_lines = c->vc_rows;

	/* We can only copy out the size of the video buffer here,
	 * otherwise we get into VGA BIOS */

	if (!vga_is_gfx) {
		vgacon_putcs(c, c->vc_screenbuf,
				c->vc_screen_size > vga_vram_size ?
				vga_vram_size : c->vc_screen_size,
				0, 0);

		if ((vgacon_xres != x || vgacon_yres != y) &&
		    (!(vga_video_num_columns % 2) &&
		     vga_video_num_columns <= screen_info.orig_video_cols &&
		     vga_video_num_lines <= rows))
			vgacon_doresize(c, c->vc_cols, c->vc_rows);
	}

	return 0;		/* Redrawing not needed */
}

static void vga_set_palette(struct vc_data *vc, const unsigned char *table)
{
	int i, j;

	vga_w(vgastate.vgabase, VGA_PEL_MSK, 0xff);
	for (i = j = 0; i < 16; i++) {
		vga_w(vgastate.vgabase, VGA_PEL_IW, table[i]);
		vga_w(vgastate.vgabase, VGA_PEL_D, vc->vc_palette[j++] >> 2);
		vga_w(vgastate.vgabase, VGA_PEL_D, vc->vc_palette[j++] >> 2);
		vga_w(vgastate.vgabase, VGA_PEL_D, vc->vc_palette[j++] >> 2);
	}
}

static void vgacon_set_palette(struct vc_data *vc, const unsigned char *table)
{
	if (vga_video_type != VIDEO_TYPE_VGAC || vga_palette_blanked
	    || !con_is_visible(vc))
		return;
	vga_set_palette(vc, table);
}

/* structure holding original VGA register settings */
static struct {
	unsigned char SeqCtrlIndex;	/* Sequencer Index reg.   */
	unsigned char CrtCtrlIndex;	/* CRT-Contr. Index reg.  */
	unsigned char CrtMiscIO;	/* Miscellaneous register */
	unsigned char HorizontalTotal;	/* CRT-Controller:00h */
	unsigned char HorizDisplayEnd;	/* CRT-Controller:01h */
	unsigned char StartHorizRetrace;	/* CRT-Controller:04h */
	unsigned char EndHorizRetrace;	/* CRT-Controller:05h */
	unsigned char Overflow;	/* CRT-Controller:07h */
	unsigned char StartVertRetrace;	/* CRT-Controller:10h */
	unsigned char EndVertRetrace;	/* CRT-Controller:11h */
	unsigned char ModeControl;	/* CRT-Controller:17h */
	unsigned char ClockingMode;	/* Seq-Controller:01h */
} vga_state;

static void vga_vesa_blank(struct vgastate *state, int mode)
{
	/* save original values of VGA controller registers */
	if (!vga_vesa_blanked) {
		raw_spin_lock_irq(&vga_lock);
		vga_state.SeqCtrlIndex = vga_r(state->vgabase, VGA_SEQ_I);
		vga_state.CrtCtrlIndex = inb_p(vga_video_port_reg);
		vga_state.CrtMiscIO = vga_r(state->vgabase, VGA_MIS_R);
		raw_spin_unlock_irq(&vga_lock);

		outb_p(0x00, vga_video_port_reg);	/* HorizontalTotal */
		vga_state.HorizontalTotal = inb_p(vga_video_port_val);
		outb_p(0x01, vga_video_port_reg);	/* HorizDisplayEnd */
		vga_state.HorizDisplayEnd = inb_p(vga_video_port_val);
		outb_p(0x04, vga_video_port_reg);	/* StartHorizRetrace */
		vga_state.StartHorizRetrace = inb_p(vga_video_port_val);
		outb_p(0x05, vga_video_port_reg);	/* EndHorizRetrace */
		vga_state.EndHorizRetrace = inb_p(vga_video_port_val);
		outb_p(0x07, vga_video_port_reg);	/* Overflow */
		vga_state.Overflow = inb_p(vga_video_port_val);
		outb_p(0x10, vga_video_port_reg);	/* StartVertRetrace */
		vga_state.StartVertRetrace = inb_p(vga_video_port_val);
		outb_p(0x11, vga_video_port_reg);	/* EndVertRetrace */
		vga_state.EndVertRetrace = inb_p(vga_video_port_val);
		outb_p(0x17, vga_video_port_reg);	/* ModeControl */
		vga_state.ModeControl = inb_p(vga_video_port_val);
		vga_state.ClockingMode = vga_rseq(state->vgabase, VGA_SEQ_CLOCK_MODE);
	}

	/* assure that video is enabled */
	/* "0x20" is VIDEO_ENABLE_bit in register 01 of sequencer */
	raw_spin_lock_irq(&vga_lock);
	vga_wseq(state->vgabase, VGA_SEQ_CLOCK_MODE, vga_state.ClockingMode | 0x20);

	/* test for vertical retrace in process.... */
	if ((vga_state.CrtMiscIO & 0x80) == 0x80)
		vga_w(state->vgabase, VGA_MIS_W, vga_state.CrtMiscIO & 0xEF);

	/*
	 * Set <End of vertical retrace> to minimum (0) and
	 * <Start of vertical Retrace> to maximum (incl. overflow)
	 * Result: turn off vertical sync (VSync) pulse.
	 */
	if (mode & VESA_VSYNC_SUSPEND) {
		outb_p(0x10, vga_video_port_reg);	/* StartVertRetrace */
		outb_p(0xff, vga_video_port_val);	/* maximum value */
		outb_p(0x11, vga_video_port_reg);	/* EndVertRetrace */
		outb_p(0x40, vga_video_port_val);	/* minimum (bits 0..3)  */
		outb_p(0x07, vga_video_port_reg);	/* Overflow */
		outb_p(vga_state.Overflow | 0x84, vga_video_port_val);	/* bits 9,10 of vert. retrace */
	}

	if (mode & VESA_HSYNC_SUSPEND) {
		/*
		 * Set <End of horizontal retrace> to minimum (0) and
		 *  <Start of horizontal Retrace> to maximum
		 * Result: turn off horizontal sync (HSync) pulse.
		 */
		outb_p(0x04, vga_video_port_reg);	/* StartHorizRetrace */
		outb_p(0xff, vga_video_port_val);	/* maximum */
		outb_p(0x05, vga_video_port_reg);	/* EndHorizRetrace */
		outb_p(0x00, vga_video_port_val);	/* minimum (0) */
	}

	/* restore both index registers */
	vga_w(state->vgabase, VGA_SEQ_I, vga_state.SeqCtrlIndex);
	outb_p(vga_state.CrtCtrlIndex, vga_video_port_reg);
	raw_spin_unlock_irq(&vga_lock);
}

static void vga_vesa_unblank(struct vgastate *state)
{
	/* restore original values of VGA controller registers */
	raw_spin_lock_irq(&vga_lock);
	vga_w(state->vgabase, VGA_MIS_W, vga_state.CrtMiscIO);

	outb_p(0x00, vga_video_port_reg);	/* HorizontalTotal */
	outb_p(vga_state.HorizontalTotal, vga_video_port_val);
	outb_p(0x01, vga_video_port_reg);	/* HorizDisplayEnd */
	outb_p(vga_state.HorizDisplayEnd, vga_video_port_val);
	outb_p(0x04, vga_video_port_reg);	/* StartHorizRetrace */
	outb_p(vga_state.StartHorizRetrace, vga_video_port_val);
	outb_p(0x05, vga_video_port_reg);	/* EndHorizRetrace */
	outb_p(vga_state.EndHorizRetrace, vga_video_port_val);
	outb_p(0x07, vga_video_port_reg);	/* Overflow */
	outb_p(vga_state.Overflow, vga_video_port_val);
	outb_p(0x10, vga_video_port_reg);	/* StartVertRetrace */
	outb_p(vga_state.StartVertRetrace, vga_video_port_val);
	outb_p(0x11, vga_video_port_reg);	/* EndVertRetrace */
	outb_p(vga_state.EndVertRetrace, vga_video_port_val);
	outb_p(0x17, vga_video_port_reg);	/* ModeControl */
	outb_p(vga_state.ModeControl, vga_video_port_val);
	/* ClockingMode */
	vga_wseq(state->vgabase, VGA_SEQ_CLOCK_MODE, vga_state.ClockingMode);

	/* restore index/control registers */
	vga_w(state->vgabase, VGA_SEQ_I, vga_state.SeqCtrlIndex);
	outb_p(vga_state.CrtCtrlIndex, vga_video_port_reg);
	raw_spin_unlock_irq(&vga_lock);
}

static void vga_pal_blank(struct vgastate *state)
{
	int i;

	vga_w(state->vgabase, VGA_PEL_MSK, 0xff);
	for (i = 0; i < 16; i++) {
		vga_w(state->vgabase, VGA_PEL_IW, i);
		vga_w(state->vgabase, VGA_PEL_D, 0);
		vga_w(state->vgabase, VGA_PEL_D, 0);
		vga_w(state->vgabase, VGA_PEL_D, 0);
	}
}

static int vgacon_blank(struct vc_data *c, int blank, int mode_switch)
{
	switch (blank) {
	case 0:		/* Unblank */
		if (vga_vesa_blanked) {
			vga_vesa_unblank(&vgastate);
			vga_vesa_blanked = 0;
		}
		if (vga_palette_blanked) {
			vga_set_palette(c, color_table);
			vga_palette_blanked = false;
			return 0;
		}
		vga_is_gfx = false;
		/* Tell console.c that it has to restore the screen itself */
		return 1;
	case 1:		/* Normal blanking */
	case -1:	/* Obsolete */
		if (!mode_switch && vga_video_type == VIDEO_TYPE_VGAC) {
			vga_pal_blank(&vgastate);
			vga_palette_blanked = true;
			return 0;
		}
		vgacon_reset_origin(c);
		scr_memset(vga_vram_base, BLANK, c->vc_screen_size);
		if (mode_switch)
			vga_is_gfx = true;
		return 1;
	default:		/* VESA blanking */
		if (vga_video_type == VIDEO_TYPE_VGAC) {
			vga_vesa_blank(&vgastate, blank - 1);
			vga_vesa_blanked = blank;
		}
		return 0;
	}
}

/*
 * PIO_FONT support.
 *
 * The font loading code goes back to the codepage package by
 * Joel Hoffman (joel@wam.umd.edu). (He reports that the original
 * reference is: "From: p. 307 of _Programmer's Guide to PC & PS/2
 * Video Systems_ by Richard Wilton. 1987.  Microsoft Press".)
 *
 * Change for certain monochrome monitors by Yury Shevchuck
 * (sizif@botik.yaroslavl.su).
 */

#define colourmap 0xa0000
/* Pauline Middelink <middelin@polyware.iaf.nl> reports that we
   should use 0xA0000 for the bwmap as well.. */
#define blackwmap 0xa0000
#define cmapsz 8192

static int vgacon_do_font_op(struct vgastate *state, char *arg, int set)
{
	int font_select = 0x00, beg, i;
	char *charmap;
	bool clear_attribs = false;
	if (vga_video_type != VIDEO_TYPE_EGAM) {
		charmap = (char *) VGA_MAP_MEM(colourmap, 0);
		beg = 0x0e;
	} else {
		charmap = (char *) VGA_MAP_MEM(blackwmap, 0);
		beg = 0x0a;
	}

	/*
	 * All fonts are loaded in slot 0
	 */

	if (!arg)
		return -EINVAL;	/* Return to default font not supported */

	raw_spin_lock_irq(&vga_lock);
	/* First, the Sequencer */
	vga_wseq(state->vgabase, VGA_SEQ_RESET, 0x1);
	/* CPU writes only to map 2 */
	vga_wseq(state->vgabase, VGA_SEQ_PLANE_WRITE, 0x04);	
	/* Sequential addressing */
	vga_wseq(state->vgabase, VGA_SEQ_MEMORY_MODE, 0x07);	
	/* Clear synchronous reset */
	vga_wseq(state->vgabase, VGA_SEQ_RESET, 0x03);

	/* Now, the graphics controller, select map 2 */
	vga_wgfx(state->vgabase, VGA_GFX_PLANE_READ, 0x02);		
	/* disable odd-even addressing */
	vga_wgfx(state->vgabase, VGA_GFX_MODE, 0x00);
	/* map start at A000:0000 */
	vga_wgfx(state->vgabase, VGA_GFX_MISC, 0x00);
	raw_spin_unlock_irq(&vga_lock);

	if (arg) {
		if (set)
			for (i = 0; i < cmapsz; i++) {
				vga_writeb(arg[i], charmap + i);
				cond_resched();
			}
		else
			for (i = 0; i < cmapsz; i++) {
				arg[i] = vga_readb(charmap + i);
				cond_resched();
			}
	}

	raw_spin_lock_irq(&vga_lock);
	/* First, the sequencer, Synchronous reset */
	vga_wseq(state->vgabase, VGA_SEQ_RESET, 0x01);	
	/* CPU writes to maps 0 and 1 */
	vga_wseq(state->vgabase, VGA_SEQ_PLANE_WRITE, 0x03);
	/* odd-even addressing */
	vga_wseq(state->vgabase, VGA_SEQ_MEMORY_MODE, 0x03);
	/* Character Map Select */
	if (set)
		vga_wseq(state->vgabase, VGA_SEQ_CHARACTER_MAP, font_select);
	/* clear synchronous reset */
	vga_wseq(state->vgabase, VGA_SEQ_RESET, 0x03);

	/* Now, the graphics controller, select map 0 for CPU */
	vga_wgfx(state->vgabase, VGA_GFX_PLANE_READ, 0x00);
	/* enable even-odd addressing */
	vga_wgfx(state->vgabase, VGA_GFX_MODE, 0x10);
	/* map starts at b800:0 or b000:0 */
	vga_wgfx(state->vgabase, VGA_GFX_MISC, beg);

	raw_spin_unlock_irq(&vga_lock);

	if (clear_attribs) {
		for (i = 0; i < MAX_NR_CONSOLES; i++) {
			struct vc_data *c = vc_cons[i].d;
			if (c && c->vc_sw == &vga_con) {
				clear_buffer_attributes(c);
			}
		}
	}
	return 0;
}

/*
 * Adjust the screen to fit a font of a certain height
 */
static int vgacon_adjust_height(struct vc_data *vc, unsigned fontheight)
{
	unsigned char ovr, vde, fsr;
	int rows, maxscan, i;

	rows = vc->vc_scan_lines / fontheight;	/* Number of video rows we end up with */
	maxscan = rows * fontheight - 1;	/* Scan lines to actually display-1 */

	/* Reprogram the CRTC for the new font size
	   Note: the attempt to read the overflow register will fail
	   on an EGA, but using 0xff for the previous value appears to
	   be OK for EGA text modes in the range 257-512 scan lines, so I
	   guess we don't need to worry about it.

	   The same applies for the spill bits in the font size and cursor
	   registers; they are write-only on EGA, but it appears that they
	   are all don't care bits on EGA, so I guess it doesn't matter. */

	raw_spin_lock_irq(&vga_lock);
	outb_p(0x07, vga_video_port_reg);	/* CRTC overflow register */
	ovr = inb_p(vga_video_port_val);
	outb_p(0x09, vga_video_port_reg);	/* Font size register */
	fsr = inb_p(vga_video_port_val);
	raw_spin_unlock_irq(&vga_lock);

	vde = maxscan & 0xff;	/* Vertical display end reg */
	ovr = (ovr & 0xbd) +	/* Overflow register */
	    ((maxscan & 0x100) >> 7) + ((maxscan & 0x200) >> 3);
	fsr = (fsr & 0xe0) + (fontheight - 1);	/*  Font size register */

	raw_spin_lock_irq(&vga_lock);
	outb_p(0x07, vga_video_port_reg);	/* CRTC overflow register */
	outb_p(ovr, vga_video_port_val);
	outb_p(0x09, vga_video_port_reg);	/* Font size */
	outb_p(fsr, vga_video_port_val);
	outb_p(0x12, vga_video_port_reg);	/* Vertical display limit */
	outb_p(vde, vga_video_port_val);
	raw_spin_unlock_irq(&vga_lock);
	vga_video_font_height = fontheight;

	for (i = 0; i < MAX_NR_CONSOLES; i++) {
		struct vc_data *c = vc_cons[i].d;

		if (c && c->vc_sw == &vga_con) {
			if (con_is_visible(c)) {
			        /* void size to cause regs to be rewritten */
				cursor_size_lastfrom = 0;
				cursor_size_lastto = 0;
				c->vc_sw->con_cursor(c, CM_DRAW);
			}
			vc_resize(c, 0, rows);	/* Adjust console size */
		}
	}
	return 0;
}

static int vgacon_font_set(struct vc_data *c, struct console_font *font,
			   unsigned int vpitch, unsigned int flags)
{
	unsigned charcount = font->charcount;
	int rc;

	if (vga_video_type < VIDEO_TYPE_EGAM)
		return -EINVAL;

	if (font->width != VGA_FONTWIDTH || font->height > 32 || vpitch != 32 ||
	    charcount != 256)
		return -EINVAL;

	rc = vgacon_do_font_op(&vgastate, font->data, 1);
	if (rc)
		return rc;

	if (!(flags & KD_FONT_FLAG_DONT_RECALC))
		rc = vgacon_adjust_height(c, font->height);
	return rc;
}

static int vgacon_font_get(struct vc_data *c, struct console_font *font, unsigned int vpitch)
{
	if (vga_video_type < VIDEO_TYPE_EGAM || vpitch != 32)
		return -EINVAL;

	font->width = VGA_FONTWIDTH;
	font->height = vga_video_font_height;
	font->charcount = 256;
	if (!font->data)
		return 0;
	return vgacon_do_font_op(&vgastate, font->data, 0);
}

static int vgacon_resize(struct vc_data *c, unsigned int width,
			 unsigned int height, unsigned int user)
{
	if (width * height > vga_vram_size)
		return -EINVAL;

	if (user) {
		/*
		 * Ho ho!  Someone (svgatextmode, eh?) may have reprogrammed
		 * the video mode!  Set the new defaults then and go away.
		 */
		screen_info.orig_video_cols = width;
		screen_info.orig_video_lines = height;
		vga_default_font_height = vga_video_font_height;
		return 0;
	}
	if (width % 2 || width > screen_info.orig_video_cols ||
	    height > (screen_info.orig_video_lines * vga_default_font_height)/
	    vga_video_font_height)
		return -EINVAL;

	if (con_is_visible(c) && !vga_is_gfx) /* who knows */
		vgacon_doresize(c, width, height);
	return 0;
}

static int vgacon_reset_origin(struct vc_data *c)
{
	if (vga_is_gfx ||	/* We don't play origin tricks in graphic modes */
	    (console_blanked && !vga_palette_blanked))	/* Nor we write to blanked screens */
		return 0;
	vga_visible_origin = vga_origin = vga_vram_base;
	vga_update_mem_top(c);
	vga_rolled_over = 0;
	return 1;
}

static bool vgacon_scroll(struct vc_data *c, unsigned int t, unsigned int b,
		enum con_scroll dir, unsigned int lines)
{
	size_t oldo;
	unsigned int delta;
	u16 erase;

	if (t || b != c->vc_rows || vga_is_gfx || c->vc_mode != KD_TEXT)
		return false;

	if (!vga_hardscroll_enabled || lines >= c->vc_rows / 2)
		return false;

	erase = to_u16(c, c->vc_video_erase);

	vgacon_restore_screen(c);
	oldo = vga_origin - vga_vram_base;
	delta = lines * c->vc_cols;
	if (dir == SM_UP) {
		if (vga_origin + c->vc_screen_size + delta >= vga_vram_end) {
			scr_memmove(vga_vram_base,
					vga_origin + delta,
					c->vc_screen_size - delta);

			vga_origin = vga_vram_base;
			vga_rolled_over = oldo;
		} else
			vga_origin += delta;

		scr_memset(vga_origin + c->vc_screen_size - delta, erase,
				delta);

		cellmove(c->vc_screenbuf, c->vc_screenbuf + delta,
				c->vc_screen_size - delta);
		cellset(c->vc_scr_end - delta, c->vc_video_erase, delta);
	} else {
		if (delta > oldo) {
			scr_memmove(vga_vram_end - c->vc_screen_size + delta,
					vga_origin,
					c->vc_screen_size - delta);

			vga_origin = vga_vram_end - c->vc_screen_size;
			vga_rolled_over = 0;
		} else
			vga_origin -= delta;

		scr_memset(vga_origin, erase, delta);

		cellmove(c->vc_screenbuf + delta,
				c->vc_screenbuf,
				c->vc_screen_size - delta);
		cellset(c->vc_screenbuf, c->vc_video_erase, delta);
	}
	vga_visible_origin = vga_origin;
	vga_update_mem_top(c);
	return true;
}

/* Used by selection. Hence it uses the visible area. */
static void vgacon_complement_pointer_pos(struct vc_data *vc, int offset)
{
	static int old_offset = -1;
	static u16 old;

	u16 complement_mask = vga_can_do_color ? 0x7700 :
		0x8000;	/* blink */

	if (old_offset != -1 && old_offset >= 0 &&
			old_offset < vc->vc_screen_size) {
		scr_write(old, vga_visible_origin + old_offset);
	}

	old_offset = offset;

	if (offset != -1 && offset >= 0 && offset < vc->vc_screen_size) {
		old = scr_read(vga_visible_origin + offset);
		scr_write(old ^ complement_mask,
				vga_visible_origin + offset);
	}
}

/* Used by selection. Hence it uses the visible area. */
static u16 vgacon_screen_glyph(const struct vc_data *vc, int ypos, int xpos)
{
	return scr_read(vga_visible_origin + ypos * vc->vc_cols + xpos);
}

static void vgacon_clear(struct vc_data *vc, int ypos, int xpos, int height,
			 int width)
{
	u16 *p = vga_origin + ypos * vc->vc_cols + xpos;

	while (height--) {
		scr_memset(p, BLANK, width);
		p += vc->vc_cols;
	}
}

static void vgacon_putc(struct vc_data *vc, struct vc_cell c, int ypos, int xpos)
{
	u16 w = to_u16(vc, c);

	u16 *p = vga_origin + ypos * vc->vc_cols + xpos;
	scr_write(w, p);
}

static void vgacon_putcs(struct vc_data *vc, const struct vc_cell *s,
			 int count, int ypos, int xpos)
{
	while (count--) {
		vgacon_putc(vc, *s++, ypos, xpos++);
		if (xpos >= vc->vc_cols) {
			ypos++;
			xpos = 0;
		}
	}
}

/*
 *  The console `switch' structure for the VGA based console
 */

const struct consw vga_con = {
	.owner = THIS_MODULE,
	.con_startup = vgacon_startup,
	.con_init = vgacon_init,
	.con_deinit = vgacon_deinit,
	.con_clear = vgacon_clear,
	.con_putc = vgacon_putc,
	.con_putcs = vgacon_putcs,
	.con_cursor = vgacon_cursor,
	.con_scroll = vgacon_scroll,
	.con_switch = vgacon_switch,
	.con_blank = vgacon_blank,
	.con_font_set = vgacon_font_set,
	.con_font_get = vgacon_font_get,
	.con_resize = vgacon_resize,
	.con_set_palette = vgacon_set_palette,
	.con_scrollback= vgacon_scrollback,
	.con_reset_origin = vgacon_reset_origin,
	.con_invert_selection = vgacon_invert_selection,
	.con_complement_pointer_pos = vgacon_complement_pointer_pos,
	.con_screen_glyph = vgacon_screen_glyph,
};
EXPORT_SYMBOL(vga_con);

MODULE_LICENSE("GPL");
