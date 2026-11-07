/*
 * G-Player: host <-> Android clipboard (text).
 *
 * QEMU listens on 127.0.0.1:<port>; GtvClip (gplayer/android/GtvClip.java,
 * started in the guest by ui/adb.c) connects through slirp's 10.0.2.2,
 * authenticates with a random token, receives every host clipboard change
 * and sends every Android clipboard change, which QEMU puts on the Windows
 * clipboard.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GTV_CLIPBOARD_H
#define GTV_CLIPBOARD_H

/* start the listener once; returns the port, 0 when it cannot listen */
int gtv_clip_start(void);

/* the token GtvClip must send ("AUTH <token>") */
const char *gtv_clip_token(void);

/* new host clipboard text (UTF-8); any thread */
void gtv_clip_host_text(const char *utf8);

#endif
