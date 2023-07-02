# SPDX-License-Identifier: GPL-2.0
#
# This file contains the font map for the default (hardware) font
#
obj-$(CONFIG_VT)			+= vt_ioctl.o vc_screen.o \
					   selection.o keyboard.o
obj-$(CONFIG_HW_CONSOLE)		+= vt.o defkeymap.o

# Files generated that shall be removed upon make clean
clean-files := defkeymap.c

$(obj)/defkeymap.o:  $(obj)/defkeymap.c

# Uncomment if you're changing the keymap and have an appropriate
# loadkeys version for the map. By default, we'll use the shipped
# versions.
# GENERATE_KEYMAP := 1

ifdef GENERATE_KEYMAP

$(obj)/defkeymap.c: $(obj)/%.c: $(src)/%.map
	loadkeys --mktable --unicode $< > $@

endif
