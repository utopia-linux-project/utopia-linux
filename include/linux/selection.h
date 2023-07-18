/* SPDX-License-Identifier: GPL-2.0 */
/*
 * selection.h
 *
 * Interface between console.c, tty_io.c, vt.c, vc_screen.c and selection.c
 */

#ifndef _LINUX_SELECTION_H_
#define _LINUX_SELECTION_H_

#include <linux/tiocl.h>

struct tty_struct;
struct vc_data;

extern void clear_selection(void);
extern int set_selection_user(const struct tiocl_selection __user *sel,
			      struct tty_struct *tty);
extern int set_selection_kernel(struct tiocl_selection *v,
				struct tty_struct *tty);
extern int paste_selection(struct tty_struct *tty);
extern int sel_loadlut(char __user *p);
extern int mouse_reporting(void);
extern void mouse_report(struct tty_struct * tty, int butt, int mrx, int mry);

bool vc_is_sel(struct vc_data *vc);

extern int console_blanked;

extern const unsigned char color_table[];
extern unsigned char default_red[];
extern unsigned char default_grn[];
extern unsigned char default_blu[];

struct vc_cell;
extern struct vc_cell *screen_pos(const struct vc_data *vc, int w_offset,
		bool viewed);
extern u16 screen_glyph(const struct vc_data *vc, int offset);
extern u32 screen_glyph_unicode(const struct vc_data *vc, int offset);
extern void complement_pointer_pos(struct vc_data *vc, int offset);
extern void invert_selection(struct vc_data *vc, int offset, int count);

extern void getconsxy(const struct vc_data *vc, unsigned char xy[static 2]);
extern void putconsxy(struct vc_data *vc, unsigned char xy[static const 2]);

extern u16 vcs_readcell(const struct vc_data *vc, const struct vc_cell *org);
extern void vcs_writecell(struct vc_data *vc, u16 val, struct vc_cell *org);
extern void vcs_scr_updated(struct vc_data *vc);

#endif
