/*
 * G-Player: only G-Player's own adb clients may reach the VM's adb.
 *
 * While adbd is reachable (boot -> game launched) a thread watches the TCP
 * table and drops connections to the VM's adb port (hostfwd) that do not come
 * from our private adb server, and connections to that adb server that do not
 * come from an adb.exe QEMU started itself.
 */
#ifndef GTV_ADBGUARD_H
#define GTV_ADBGUARD_H

#include <windows.h>

/* fwd_port: hostfwd to the guest's adbd; server_port: our adb server */
void gtv_adbguard_start(int fwd_port, int server_port);
void gtv_adbguard_stop(void);

/* adb.exe clients started by QEMU: allowed to talk to our adb server */
void gtv_adbguard_allow_pid(DWORD pid);
void gtv_adbguard_forget_pid(DWORD pid);

/* PID listening on server_port (our adb server), 0 if none */
DWORD gtv_adbguard_server_pid(void);

#endif
