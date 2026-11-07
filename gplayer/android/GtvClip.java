/*
 * GtvClip: host <-> Android clipboard (text) for G-Player.
 *
 * Runs inside the guest with app_process (no APK, no install):
 *   CLASSPATH=/data/local/tmp/gtvclip.dex app_process / GtvClip <port> <token> [game package]
 * ui/adb.c (QEMU) pushes the dex and starts it on every boot.
 *
 * It connects to QEMU through slirp (10.0.2.2 = the host's 127.0.0.1) and
 * sends "AUTH <token>".  Then, one line per change, base64 of UTF-8 text:
 *   host -> guest  "SET <b64>"   put on the Android clipboard (Ctrl+V / Paste)
 *   guest -> host  "CLIP <b64>"  the Android clipboard changed (copy in game)
 * The Android clipboard is polled every POLL_MS; a text that came from the
 * host is not sent back.  Android APIs are reached by reflection, so this
 * file builds with a plain JDK + d8, without the Android SDK (build-dex.sh).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

public final class GtvClip {
    private static final String HOST = "10.0.2.2";
    private static final String PKG = "com.android.shell";
    private static final int POLL_MS = 500;
    private static final int MAX_CHARS = 512 * 1024;  /* larger copies are not sent */
    /* SystemUI (Android 13+) does not pop its clipboard editor for such clips */
    private static final String SUPPRESS_OVERLAY =
        "com.android.systemui.SUPPRESS_CLIPBOARD_OVERLAY";

    private static Object clipboard;
    private static Method setPrimaryClip;
    private static Method getPrimaryClip;
    private static Class<?> clipDataClass;

    /* last text seen on / put on the Android clipboard, under GtvClip.class */
    private static String lastSeen;
    private static boolean canRead;

    private static final int SHELL_UID = 2000;

    private static boolean becomeShell() {
        try {
            Class<?> os = Class.forName("android.system.Os");
            if ((Integer) os.getMethod("getuid").invoke(null) != SHELL_UID) {
                os.getMethod("setgid", int.class).invoke(null, SHELL_UID);
                os.getMethod("setuid", int.class).invoke(null, SHELL_UID);
            }
            int uid = (Integer) os.getMethod("getuid").invoke(null);
            log("running as uid " + uid);
            return uid == SHELL_UID;
        } catch (Throwable t) {
            log("cannot become shell, guest -> host off: " + t);
            return false;
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: GtvClip <port> <token>");
            System.exit(2);
        }
        int port = Integer.parseInt(args[0]);
        String token = args[1];
        final String gamePkg = args.length > 2 ? args[2] : null;

        /*
         * Started as root (to leave adbd's cgroup).  Reading the clipboard as
         * root with PKG makes ClipboardService fail its "uid owns package"
         * check and CLEAR the clipboard, so become shell (uid 2000, owner of
         * com.android.shell) first; the guest -> host direction is off when
         * that fails.  Threads started later inherit the uid.
         */
        canRead = becomeShell();

        /* what is on the clipboard at start is not a new copy */
        if (canRead) {
            try {
                lastSeen = getClip();
            } catch (Throwable t) {
                log("get failed: " + t);
                clipboard = null;
            }
        }

        /* what the player does: one reader for the whole boot (see startUxReader) */
        startUxReader(gamePkg);

        for (;;) {
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress(HOST, port), 3000);
                s.setKeepAlive(true);
                OutputStream out = s.getOutputStream();
                send(out, "AUTH " + token);
                log("connected to " + HOST + ":" + port);

                Thread poller = canRead ? startPoller(s, out) : null;
                Thread stats = startStats(s, out, gamePkg);
                Thread ux = startUxSender(s, out);
                try {
                    readLoop(s);
                } finally {
                    if (poller != null) {
                        poller.interrupt();
                    }
                    stats.interrupt();
                    ux.interrupt();
                }
            } catch (Exception e) {
                /* QEMU not listening (yet) or connection closed: retry */
            }
            Thread.sleep(2000);
        }
    }

    /* host -> guest */
    private static void readLoop(Socket s) throws Exception {
        BufferedReader in = new BufferedReader(
            new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
        String line;
        while ((line = in.readLine()) != null) {
            if (!line.startsWith("SET ")) {
                continue;
            }
            String text = new String(Base64.getDecoder().decode(line.substring(4)),
                                     StandardCharsets.UTF_8);
            try {
                synchronized (GtvClip.class) {
                    lastSeen = text;            /* do not echo it back */
                    setClip(text);
                }
                log("clipboard set (" + text.length() + " chars)");
            } catch (Throwable t) {
                log("set failed: " + t);
                clipboard = null;               /* look the service up again */
            }
        }
    }

    /* guest -> host */
    private static Thread startPoller(final Socket s, final OutputStream out) {
        Thread t = new Thread(new Runnable() {
            public void run() {
                while (!Thread.currentThread().isInterrupted() && !s.isClosed()) {
                    try {
                        Thread.sleep(POLL_MS);
                    } catch (InterruptedException e) {
                        return;
                    }
                    String text;
                    try {
                        synchronized (GtvClip.class) {
                            text = getClip();
                            if (text == null || text.isEmpty() || text.equals(lastSeen)) {
                                continue;
                            }
                            lastSeen = text;
                        }
                    } catch (Throwable e) {
                        log("get failed: " + e);
                        clipboard = null;
                        continue;
                    }
                    if (text.length() > MAX_CHARS) {
                        log("guest copy too large (" + text.length() + " chars), not sent");
                        continue;
                    }
                    try {
                        send(out, "CLIP " + Base64.getEncoder().encodeToString(
                            text.getBytes(StandardCharsets.UTF_8)));
                        log("sent guest copy (" + text.length() + " chars)");
                    } catch (Exception e) {
                        try {
                            s.close();          /* wakes readLoop up */
                        } catch (Exception ignored) {
                        }
                        return;
                    }
                }
            }
        }, "gtvclip-poll");
        t.setDaemon(true);
        t.start();
        return t;
    }

    /*
     * Guest load for the HUD / [PERF] log in QEMU (ui/gtv-perf.c), every 2 s:
     *   STAT cpu=<guest %> game=<game process %> avail=<MB> swap=<MB used>
     * Percentages are of the whole guest (all vCPUs); game=-1 when the game
     * process is not found.
     */
    private static final int STAT_MS = 2000;

    private static Thread startStats(final Socket s, final OutputStream out, final String pkg) {
        Thread t = new Thread(new Runnable() {
            public void run() {
                long[] prev = cpuTicks();
                long prevGame = -1;
                int gamePid = -1;
                int bridge = -1;            /* 1: ARM code through ndk_translation */
                while (!Thread.currentThread().isInterrupted() && !s.isClosed()) {
                    try {
                        Thread.sleep(STAT_MS);
                    } catch (InterruptedException e) {
                        return;
                    }
                    try {
                        long[] cur = cpuTicks();
                        long total = cur[0] - prev[0], busy = total - (cur[1] - prev[1]);
                        int cpu = total > 0 ? (int) (busy * 100 / total) : 0;

                        if (pkg != null && (gamePid < 0 || !new java.io.File("/proc/" + gamePid).exists())) {
                            gamePid = findPid(pkg);
                            prevGame = -1;
                            if (gamePid > 0 && bridge < 0) {
                                bridge = usesArmTranslation(pkg);
                            }
                        }
                        long g = gamePid > 0 ? procTicks(gamePid) : -1;
                        int game = -1;
                        if (g >= 0 && prevGame >= 0 && total > 0) {
                            game = (int) ((g - prevGame) * 100 / total);
                        }
                        prevGame = g;
                        prev = cur;

                        long[] mem = memInfo();
                        send(out, "STAT cpu=" + cpu + " game=" + game +
                             " avail=" + mem[0] / 1024 + " swap=" + mem[1] / 1024 +
                             " arm=" + bridge);
                    } catch (Exception e) {
                        return;                     /* connection gone */
                    }
                }
            }
        }, "gtvclip-stat");
        t.setDaemon(true);
        t.start();
        return t;
    }

    /*
     * What the player does, for the user-activity log (QEMU ui/gtv-ux.c):
     *   UX <epoch ms> FOCUS <window>    window that got input focus (activity,
     *                                   dialog), from the "input_focus" event
     *   UX <epoch ms> ANR <package>     "Application Not Responding" dialog
     *   UX <epoch ms> DH <EVENT>        event name of the DH platform SDK
     *                                   (DHLogger: GAME_CLIENT_SPLASH_100,
     *                                   SDK_CLIENT_LOGIN_START, ...)
     *   UX <epoch ms> VTC <label>       label of a VTC login SDK line (mytag:
     *                                   "RegisterTime Start", ...)
     *   UX <epoch ms> CRASH <class>     the game died: exception class only
     * Only names: never field contents, payloads, typed text or values (a
     * mytag line keeps the words before ':' and one allowed word after it).
     * logcat replays the buffers from boot, so events from before GtvClip
     * started (game splash/loading) are sent too; lines wait in a queue while
     * QEMU is not connected.
     */
    private static final java.util.concurrent.LinkedBlockingDeque<String> uxQueue =
        new java.util.concurrent.LinkedBlockingDeque<String>(4000);
    private static final java.util.regex.Pattern LOGCAT_LINE = java.util.regex.Pattern.compile(
        "^\\s*(\\d+)\\.(\\d{3})\\s+\\d+\\s+\\d+\\s+[VDIWEF]\\s+([^:]+?)\\s*: (.*)$");
    private static final java.util.regex.Pattern DH_EVENT =
        java.util.regex.Pattern.compile("\\d+::([A-Z][A-Z0-9_]{2,63})");
    private static final java.util.regex.Pattern VTC_LABEL =
        java.util.regex.Pattern.compile("^([A-Za-z][A-Za-z ]{0,40}?)\\s*:\\s*([A-Za-z]+)?");
    private static final java.util.Set<String> VTC_WORDS = new java.util.HashSet<String>(
        java.util.Arrays.asList("Start", "End", "Success", "Succeed", "Fail", "Failed",
                                "Error", "Cancel", "Done", "OK", "Begin", "Finish"));
    private static final java.util.regex.Pattern FOCUS =
        java.util.regex.Pattern.compile("Focus entering \\S+ (.+?) \\((server|client)\\)");

    private static void ux(long ms, String kind, String text) {
        /* names stay in UTF-8 (dialog titles like "Thông báo"); only control
         * characters go. QEMU reads at most ~190 bytes of text. */
        text = text.replaceAll("\\p{Cntrl}", "?");
        if (text.length() > 160) {
            text = text.substring(0, 160);
        }
        while (text.getBytes(StandardCharsets.UTF_8).length > 180) {
            text = text.substring(0, text.length() - 8);
        }
        uxQueue.offer("UX " + ms + " " + kind + " " + text);
    }

    private static void startUxReader(final String pkg) {
        Thread t = new Thread(new Runnable() {
            public void run() {
                for (;;) {
                    try {
                        Process p = new ProcessBuilder("logcat", "-v", "epoch",
                            "-b", "main,events,crash",
                            "-s", "input_focus:I", "DHLogger:D", "mytag:D", "AndroidRuntime:E")
                            .redirectErrorStream(true).start();
                        BufferedReader r = new BufferedReader(
                            new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8));
                        String line;
                        int crash = 0;      /* 1: FATAL seen, 2: it is the game */
                        while ((line = r.readLine()) != null) {
                            java.util.regex.Matcher m = LOGCAT_LINE.matcher(line);
                            if (!m.matches()) {
                                continue;
                            }
                            long ms = Long.parseLong(m.group(1)) * 1000 + Integer.parseInt(m.group(2));
                            String tag = m.group(3), msg = m.group(4);
                            if (tag.equals("input_focus")) {
                                java.util.regex.Matcher f = FOCUS.matcher(msg);
                                if (f.find()) {
                                    String w = f.group(1);
                                    if (w.startsWith("Application Not Responding: ")) {
                                        ux(ms, "ANR", w.substring(28));
                                    } else {
                                        ux(ms, "FOCUS", w);
                                    }
                                }
                            } else if (tag.equals("DHLogger")) {
                                java.util.regex.Matcher d = DH_EVENT.matcher(msg);
                                if (d.find()) {
                                    ux(ms, "DH", d.group(1));
                                }
                            } else if (tag.equals("mytag")) {
                                java.util.regex.Matcher v = VTC_LABEL.matcher(msg);
                                if (v.find()) {
                                    String w = v.group(2);
                                    ux(ms, "VTC", v.group(1).trim() +
                                       (w != null && VTC_WORDS.contains(w) ? " " + w : ""));
                                }
                            } else if (tag.equals("AndroidRuntime")) {
                                if (msg.startsWith("FATAL EXCEPTION")) {
                                    crash = 1;
                                } else if (crash == 1 && msg.startsWith("Process: ")) {
                                    crash = pkg != null && msg.startsWith("Process: " + pkg + ",") ? 2 : 0;
                                } else if (crash == 2) {
                                    int c = msg.indexOf(':');
                                    ux(ms, "CRASH", (c > 0 ? msg.substring(0, c) : msg).trim());
                                    crash = 0;
                                }
                            }
                        }
                        p.waitFor();
                    } catch (Exception e) {
                        log("ux reader: " + e);
                    }
                    try {
                        Thread.sleep(5000);         /* logcat died: start it again */
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        }, "gtvclip-ux");
        t.setDaemon(true);
        t.start();
    }

    private static Thread startUxSender(final Socket s, final OutputStream out) {
        Thread t = new Thread(new Runnable() {
            public void run() {
                while (!Thread.currentThread().isInterrupted() && !s.isClosed()) {
                    String line;
                    try {
                        line = uxQueue.take();
                    } catch (InterruptedException e) {
                        return;
                    }
                    try {
                        send(out, line);
                    } catch (Exception e) {
                        uxQueue.offerFirst(line);   /* for the next connection */
                        return;
                    }
                }
            }
        }, "gtvclip-uxsend");
        t.setDaemon(true);
        t.start();
        return t;
    }

    private static String readFile(String path) throws java.io.IOException {
        return new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get(path)),
                          StandardCharsets.UTF_8);
    }

    /* {total, idle} jiffies of all CPUs */
    private static long[] cpuTicks() {
        try {
            String line = readFile("/proc/stat").split("\n", 2)[0];
            String[] f = line.trim().split("\\s+");
            long total = 0;
            for (int i = 1; i < f.length; i++) {
                total += Long.parseLong(f[i]);
            }
            long idle = Long.parseLong(f[4]) + (f.length > 5 ? Long.parseLong(f[5]) : 0);
            return new long[] {total, idle};
        } catch (Exception e) {
            return new long[] {0, 0};
        }
    }

    /* utime + stime of a process, -1 when gone */
    private static long procTicks(int pid) {
        try {
            String st = readFile("/proc/" + pid + "/stat");
            String[] f = st.substring(st.lastIndexOf(')') + 2).split(" ");
            return Long.parseLong(f[11]) + Long.parseLong(f[12]);
        } catch (Exception e) {
            return -1;
        }
    }

    /*
     * 1 when the game runs ARM native code through the ARM -> x86 translator
     * (no x86 libraries in its APK): every new code path is translated on
     * first use, which shows as stalls when scenes/menus open.  0 native x86,
     * -1 unknown.
     */
    private static int usesArmTranslation(String pkg) {
        /* /proc/<pid>/maps of an app is not readable as shell: ask PackageManager */
        try {
            Process p = Runtime.getRuntime().exec(new String[] {"dumpsys", "package", pkg});
            BufferedReader r = new BufferedReader(
                new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8));
            String line;
            int result = -1;
            while ((line = r.readLine()) != null) {
                int i = line.indexOf("primaryCpuAbi=");
                if (i >= 0 && result < 0) {
                    String abi = line.substring(i + 14).trim();
                    /* "null": no native code at all, nothing to translate */
                    result = abi.startsWith("arm") ? 1 : 0;
                }
            }
            p.waitFor();
            return result;
        } catch (Exception e) {
            return -1;
        }
    }

    private static int findPid(String pkg) {
        java.io.File[] dirs = new java.io.File("/proc").listFiles();
        if (dirs == null) {
            return -1;
        }
        for (java.io.File d : dirs) {
            String n = d.getName();
            if (n.isEmpty() || !Character.isDigit(n.charAt(0))) {
                continue;
            }
            try {
                String cmd = readFile("/proc/" + n + "/cmdline");
                int z = cmd.indexOf('\0');
                if ((z < 0 ? cmd : cmd.substring(0, z)).equals(pkg)) {
                    return Integer.parseInt(n);
                }
            } catch (Exception ignored) {
            }
        }
        return -1;
    }

    /* {MemAvailable, swap used} in kB */
    private static long[] memInfo() {
        long avail = 0, swapTotal = 0, swapFree = 0;
        try {
            for (String l : readFile("/proc/meminfo").split("\n")) {
                String[] f = l.split("\\s+");
                if (f.length < 2) {
                    continue;
                }
                if (f[0].equals("MemAvailable:")) {
                    avail = Long.parseLong(f[1]);
                } else if (f[0].equals("SwapTotal:")) {
                    swapTotal = Long.parseLong(f[1]);
                } else if (f[0].equals("SwapFree:")) {
                    swapFree = Long.parseLong(f[1]);
                }
            }
        } catch (Exception ignored) {
        }
        return new long[] {avail, swapTotal - swapFree};
    }

    private static void send(OutputStream out, String line) throws Exception {
        synchronized (out) {
            out.write((line + "\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
        }
    }

    private static void lookup() throws Exception {
        if (clipboard != null) {
            return;
        }
        Class<?> sm = Class.forName("android.os.ServiceManager");
        Object binder = sm.getMethod("getService", String.class).invoke(null, "clipboard");
        Class<?> ibinder = Class.forName("android.os.IBinder");
        Class<?> stub = Class.forName("android.content.IClipboard$Stub");
        Object cb = stub.getMethod("asInterface", ibinder).invoke(null, binder);
        setPrimaryClip = null;
        getPrimaryClip = null;
        for (Method m : cb.getClass().getMethods()) {
            if (m.getName().equals("setPrimaryClip")) {
                setPrimaryClip = m;
            } else if (m.getName().equals("getPrimaryClip")) {
                getPrimaryClip = m;
            }
        }
        if (setPrimaryClip == null || getPrimaryClip == null) {
            throw new NoSuchMethodException("IClipboard.set/getPrimaryClip");
        }
        clipDataClass = Class.forName("android.content.ClipData");
        clipboard = cb;
    }

    /*
     * The IClipboard signatures change between Android versions, e.g.
     *   13: setPrimaryClip(ClipData, String callingPackage, String attributionTag, int userId)
     *       getPrimaryClip(String callingPackage, String attributionTag, int userId)
     *   14: (..., int userId, int deviceId)
     * so fill the arguments by type: the clip, the calling package, null for
     * the other strings, 0 for user/device ids.
     */
    private static Object[] args(Method m, Object clip) {
        Class<?>[] types = m.getParameterTypes();
        Object[] values = new Object[types.length];
        boolean pkgSet = false;
        for (int i = 0; i < types.length; i++) {
            if (types[i] == clipDataClass) {
                values[i] = clip;
            } else if (types[i] == String.class) {
                values[i] = pkgSet ? null : PKG;
                pkgSet = true;
            } else if (types[i] == int.class) {
                values[i] = 0;
            } else {
                values[i] = null;
            }
        }
        return values;
    }

    private static void setClip(String text) throws Exception {
        lookup();
        Object clip = clipDataClass.getMethod("newPlainText", CharSequence.class, CharSequence.class)
            .invoke(null, "G-Player", text);

        /* best effort: no SystemUI clipboard popup over the game */
        try {
            Object desc = clipDataClass.getMethod("getDescription").invoke(clip);
            Class<?> pb = Class.forName("android.os.PersistableBundle");
            Object extras = pb.getConstructor().newInstance();
            pb.getMethod("putBoolean", String.class, boolean.class).invoke(extras, SUPPRESS_OVERLAY, true);
            desc.getClass().getMethod("setExtras", pb).invoke(desc, extras);
        } catch (Throwable ignored) {
        }

        setPrimaryClip.invoke(clipboard, args(setPrimaryClip, clip));
    }

    /* text of the first clip item, or null */
    private static String getClip() throws Exception {
        lookup();
        Object clip = getPrimaryClip.invoke(clipboard, args(getPrimaryClip, null));
        if (clip == null) {
            return null;
        }
        if ((Integer) clipDataClass.getMethod("getItemCount").invoke(clip) < 1) {
            return null;
        }
        Object item = clipDataClass.getMethod("getItemAt", int.class).invoke(clip, 0);
        Object text = item.getClass().getMethod("getText").invoke(item);
        return text == null ? null : text.toString();
    }

    private static void log(String msg) {
        System.out.println("[GtvClip] " + msg);
        System.out.flush();
    }
}
