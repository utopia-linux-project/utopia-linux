/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VT_KERN_H
#define _VT_KERN_H

/*
 * this really is an extension of the vc_cons structure in console.c, but
 * with information needed by the vt package
 */

#include <linux/vt.h>
#include <linux/kd.h>
#include <linux/tty.h>
#include <linux/mutex.h>
#include <linux/console_struct.h>
#include <linux/mm.h>
#include <linux/notifier.h>

void kd_mksound(unsigned int hz, unsigned int ticks);
int kbd_rate(struct kbd_repeat *rep);

extern int fg_console, last_console, want_console;

/* console.c */

int vc_allocate(unsigned int console);
int vc_cons_allocated(unsigned int console);
int vc_resize(struct vc_data *vc, unsigned int cols, unsigned int lines);
struct vc_data *vc_deallocate(unsigned int console);
void reset_palette(struct vc_data *vc);
void do_blank_screen(int entering_gfx);
void do_unblank_screen(int leaving_gfx);
void poke_blanked_console(void);
int con_font_op(struct vc_data *vc, struct console_font_op *op);
int con_set_cmap(unsigned char __user *cmap);
int con_get_cmap(unsigned char __user *cmap);
void scrollback_normal(struct vc_data *vc);
void unscrollback(struct vc_data *vc, int lines);
void clear_buffer_attributes(struct vc_data *vc);
void update_region(struct vc_data *vc, unsigned long start, int count);
void redraw_screen(struct vc_data *vc, int is_switch);
#define update_screen(x) redraw_screen(x, 0)
#define switch_screen(x) redraw_screen(x, 1)

struct tty_struct;
int tioclinux(struct tty_struct *tty, unsigned long arg);

/* vt.c */
void vt_event_post(unsigned int event, unsigned int old, unsigned int new);
int vt_waitactive(int n);
void change_console(struct vc_data *new_vc);
void reset_vc(struct vc_data *vc);
int do_unbind_con_driver(const struct consw *csw, int first, int last,
			 int deflt);
int vty_init(const struct file_operations *console_fops);

extern bool vt_dont_switch;
extern int default_utf8;
extern int global_cursor_default;

struct vt_spawn_console {
	spinlock_t lock;
	struct pid *pid;
	int sig;
};
extern struct vt_spawn_console vt_spawn_con;

int vt_move_to_console(unsigned int vt, int alloc);

/* Interfaces for VC notification of character events (for accessibility etc) */

struct vt_notifier_param {
	struct vc_data *vc;	/* VC on which the update happened */
	unsigned int c;		/* Printed char */
};

int register_vt_notifier(struct notifier_block *nb);
int unregister_vt_notifier(struct notifier_block *nb);

void hide_boot_cursor(bool hide);

/* keyboard  provided interfaces */
int vt_do_diacrit(unsigned int cmd, void __user *up, int eperm);
int vt_do_kdskbmode(unsigned int console, unsigned int arg);
int vt_do_kdskbmeta(unsigned int console, unsigned int arg);
int vt_do_kbkeycode_ioctl(int cmd, struct kbkeycode __user *user_kbkc,
			  int perm);
int vt_do_kdsk_ioctl(int cmd, struct kbentry __user *user_kbe, int perm,
		     unsigned int console);
int vt_do_kdgkb_ioctl(int cmd, struct kbsentry __user *user_kdgkb, int perm);
int vt_do_kdskled(unsigned int console, int cmd, unsigned long arg, int perm);
int vt_do_kdgkbmode(unsigned int console);
int vt_do_kdgkbmeta(unsigned int console);
int vt_get_shift_state(void);
void vt_reset_keyboard(unsigned int console);
int vt_get_leds(unsigned int console, int flag);
int vt_get_kbd_mode_bit(unsigned int console, int bit);
void vt_set_kbd_mode_bit(unsigned int console, int bit);
void vt_clr_kbd_mode_bit(unsigned int console, int bit);
void vt_set_led_state(unsigned int console, int leds);
void vt_kbd_con_start(unsigned int console);
void vt_kbd_con_stop(unsigned int console);

void vc_scrollback_helper(struct vc_data *c, int lines,
		unsigned int rolled_over, void *_base, unsigned int size);

#endif /* _VT_KERN_H */
