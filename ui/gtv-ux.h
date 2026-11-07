/*
 * G-Player: user-activity log (screens, game SDK events, input, AFK, how the
 * session ended) as "[UX] ..." lines on stdout; the launcher writes them to
 * its log file and sends them as events (log/logEventManager.js).
 */
#ifndef GTV_UX_H
#define GTV_UX_H

/* when the window opens: session start, AFK timer */
void gtv_ux_start(void);

/* "UX <epoch ms> <KIND> <text>" from GtvClip, without the "UX " */
void gtv_ux_line(const char *line);

/* the player clicked/tapped (0) or pressed a key (1) in the window */
void gtv_ux_input(int key);

/* the session ends (window closed ...): one summary line, once */
void gtv_ux_end(const char *reason);

#endif
