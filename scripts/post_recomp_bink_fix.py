"""Re-apply Lamborghini fixes after regenerating src/recomp/gen:
  - BinkWait (0x001A6BF0) wrapper: Bink's own pacing by default, RECOMP_BINK_PACE=1 forces 25 fps
  - never detach the D3D pushbuffer (sub_0015B860 reads switch 0x16A9A0)
  - skip the ~3 s storage-message hold (RECOMP_SAVE_HOLD=1 keeps it)
Usage: python scripts/post_recomp_bink_fix.py src/recomp/gen"""
import glob, os, sys
gen = sys.argv[1] if len(sys.argv) > 1 else "src/recomp/gen"
for p in glob.glob(os.path.join(gen, "recomp_*.c")):
    s = open(p).read()
    if "void sub_001A6BF0(void)\n{" not in s or "sub_001A6BF0_orig" in s:
        continue
    s = s.replace("void sub_001A6BF0(void)\n{", "static void sub_001A6BF0_orig(void)\n{", 1)
    s += '''
/* Lamborghini: pace Bink frames at 25 fps on the host-scaled 733 MHz TSC. */
void sub_001A6BF0(void)
{
    static uint64_t last;
    uint64_t now;
    static int mode = -1;
    sub_001A6BF0_orig();
    /* Bink's own pacing is right (the videos are 25 fps interlaced, so 50
     * field updates a second). The forced 25 fps cadence below throttled
     * them into stop-start bursts; RECOMP_BINK_PACE=1 brings it back. */
    if (mode < 0) { const char *e = getenv("RECOMP_BINK_PACE"); mode = e ? atoi(e) : 0; }
    if (mode == 0) return;                /* Bink's own answer */
    now = xbox_ReadTimeStampCounter();
    /* A fixed 40 ms cadence: the next frame is due 40 ms after the last
     * one was DUE, not after it was noticed. Measuring from "noticed" added
     * up to a frame of display latency (60 Hz flip hold) to every frame, so
     * 25 fps video ran at ~20 fps with uneven gaps. A stall of more than
     * three frames restarts the clock instead of racing to catch up. */
    if (last == 0 || now - last >= 4ull * 29333333ull) { last = now; eax = 0; }
    else if (now - last >= 29333333ull) { last += 29333333ull; eax = 0; }
    else eax = 1;
}
'''
    open(p, "w").write(s)
    print("patched", p)

for p in glob.glob(os.path.join(gen, "recomp_*.c")):
    s = open(p).read()
    if "    eax = MEM32(0x16A9A0);" in s:
        s = s.replace("    eax = MEM32(0x16A9A0);",
                      "    eax = 0; /* Lamborghini: never detach the pushbuffer */", 1)
        open(p, "w").write(s)
        print("patched detach switch in", p)

# Storage messages ("Loading <profile>. Please don't turn off your Xbox console",
# saving): the game busy-waits until each has been up for its minimum time
# (~3 s) after the work is already done, with no frames drawn. Skip that hold;
# RECOMP_SAVE_HOLD=1 keeps it.
for p in glob.glob(os.path.join(gen, "recomp_*.c")):
    s = open(p).read()
    n = 0
    for loc, done in (("0003C204", "0003C222"), ("0003C2DC", "0003C2F4")):
        old = "loc_%s: ;\n    esi = ebx + 0x130;" % loc
        if old in s:
            s = s.replace(old, "loc_%s: ;\n    if (!lambo_keep_save_hold()) goto loc_%s;\n    esi = ebx + 0x130;" % (loc, done))
            n += 1
    if n:
        if "static int lambo_keep_save_hold" not in s:
            i = s.index("void sub_0003C150(void)")
            i = s.rindex("/**", 0, i)
            s = s[:i] + '''/* Lamborghini: skip the storage-message minimum display hold (RECOMP_SAVE_HOLD=1 keeps it). */
static int lambo_keep_save_hold(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_SAVE_HOLD"); v = e && atoi(e); }
    return v;
}

''' + s[i:]
        open(p, "w").write(s)
        print("patched storage-message hold (%d) in" % n, p)

# Widescreen: cameras drawing to the screen get a wider horizontal field of
# view (same vertical one), for the projection (sub_000588D0, camera in eax)
# and the culling frustum (sub_00058400, camera in esi). The frame stays
# 640x480 (anamorphic, as Xbox widescreen titles do) and the host stretches it
# to the screen. xbox_g_aspect_stretch = (screen aspect) / (4:3), 1 = off.
WIDE = '''
/* Lamborghini widescreen (post_recomp_bink_fix.py). */
extern volatile float xbox_g_aspect_stretch;
static int lambo_wide_cam(uint32_t cam)
{
    /* A screen-wide viewport (x, y, w, h at +4..+0x10): the race view and
     * the split-screen halves. The mirror, a small window laid out with the
     * HUD, keeps its view (the host squeezes it with the HUD), as do
     * off-screen cameras (reflection cube faces, 90 degrees, no viewport). */
    uint32_t x = MEM32(cam + 4), y = MEM32(cam + 8), w = MEM32(cam + 0xC), h = MEM32(cam + 0x10);
    float fx = MEMF(cam + 0x98);
    return w >= 600 && h >= 32 && x + w <= 640 && y + h <= 480 && fx > 0.05f && fx < 1.5f;
}
static float lambo_wide_fx(float fx, float a)
{
    return 2.0f * atanf(a * tanf(fx * 0.5f));
}
#define LAMBO_WIDE_WRAP(name, reg) \\
void name(void) \\
{ \\
    float a = xbox_g_aspect_stretch; \\
    uint32_t cam = reg; \\
    if (a > 1.001f && lambo_wide_cam(cam)) { \\
        float fx = MEMF(cam + 0x98); \\
        MEMF(cam + 0x98) = lambo_wide_fx(fx, a); \\
        name##_orig(); \\
        if (MEMF(cam + 0x98) == lambo_wide_fx(fx, a)) MEMF(cam + 0x98) = fx; \\
    } else \\
        name##_orig(); \\
}
'''
for p in glob.glob(os.path.join(gen, "recomp_*.c")):
    s = open(p).read()
    done = []
    for fn, reg in (("sub_000588D0", "eax"), ("sub_00058400", "esi")):
        if ("void %s(void)\n{" % fn) in s and (fn + "_orig") not in s:
            s = s.replace("void %s(void)\n{" % fn, "static void %s_orig(void)\n{" % fn, 1)
            done.append((fn, reg))
    if done:
        if "lambo_wide_cam" not in s:
            s += WIDE
        for fn, reg in done:
            s += "LAMBO_WIDE_WRAP(%s, %s)\n" % (fn, reg)
        open(p, "w").write(s)
        print("patched widescreen cameras", [f for f, _ in done], "in", p)

# Unlock all cars (F1 menu / unlock_cars= / RECOMP_UNLOCK_CARS): car select
# counts its cars with sub_000A0530 (sum of the unlocked-per-series counters
# at 0x1DAE38). When the option is on, first raise them to the per-series
# totals at 0x1DAE20, as the game's own MRS MIGGINS name cheat (sub_000A05C0)
# does. The profile saves the counters, so the unlock stays.
CARS = '''
/* Lamborghini unlock-all-cars (post_recomp_bink_fix.py). */
extern volatile int xbox_g_unlock_cars;
void sub_000A0530(void)
{
    if (xbox_g_unlock_cars) {
        int i;
        for (i = 0; i < 6; i++)
            if (MEM32(0x1DAE38 + 4 * i) < MEM32(0x1DAE20 + 4 * i))
                MEM32(0x1DAE38 + 4 * i) = MEM32(0x1DAE20 + 4 * i);
    }
    sub_000A0530_orig();
}
'''
for p in glob.glob(os.path.join(gen, "recomp_*.c")):
    s = open(p).read()
    fn = "sub_000A0530"
    if ("void %s(void)\n{" % fn) in s and (fn + "_orig") not in s:
        s = s.replace("void %s(void)\n{" % fn, "static void %s_orig(void)\n{" % fn, 1)
        s += CARS
        open(p, "w").write(s)
        print("patched unlock-all-cars in", p)

# Unlock all tracks (F1 menu / unlock_tracks= / RECOMP_UNLOCK_TRACKS): the
# track table at 0x1F9E74 holds 10 tracks of 0x20 bytes, a lock flag at +0
# and six route locks at +4..+0x18 (nonzero = locked). Arcade track select
# reads it through sub_000A03B0 / 03E0 / 0410 / 0440 / 04C0 / 04E0. When the
# option is on, each of them first runs sub_000A0560 -- the game's own
# "unlock everything" (the DR CUNNING name cheat, and what winning a
# championship calls), which clears exactly the routes each track has --
# once, and again only when the game rewrites the table (a profile load).
# The profile saves the table, so the unlock stays.
TRACKS_HELPER = """
/* Lamborghini unlock-all-tracks (post_recomp_bink_fix.py). */
#include <string.h>
extern volatile int xbox_g_unlock_tracks;
static void lambo_unlock_tracks(void)
{
    static uint8_t snap[10 * 0x20];
    static int have;
    uint32_t i, locked = 0;
    if (!xbox_g_unlock_tracks)
        return;
    for (i = 0; i < 10; i++)
        locked |= MEM32(0x1F9E74 + 0x20 * i);
    if (!locked && have && !memcmp(snap, (const void *)XBOX_PTR(0x1F9E74), sizeof snap))
        return;
    {
        uint32_t s_ebp = g_ebp, s_seh = g_seh_ebp, s_ebx = ebx, s_esi = esi, s_edi = edi;
        uint32_t s_eax = eax, s_ecx = ecx, s_edx = edx, s_esp = esp;
        PUSH32(esp, 0);                 /* return address, popped by its ret */
        sub_000A0560();
        esp = s_esp; g_ebp = s_ebp; g_seh_ebp = s_seh; ebx = s_ebx; esi = s_esi; edi = s_edi;
        eax = s_eax; ecx = s_ecx; edx = s_edx;
    }
    memcpy(snap, (const void *)XBOX_PTR(0x1F9E74), sizeof snap);
    have = 1;
}
"""
TRACKS_WRAP = """void %s(void)
{
    lambo_unlock_tracks();
    %s_orig();
}
"""
for p in glob.glob(os.path.join(gen, "recomp_*.c")):
    s = open(p).read()
    done = []
    for fn in ("sub_000A03B0", "sub_000A03E0", "sub_000A0410", "sub_000A0440",
               "sub_000A04C0", "sub_000A04E0"):
        if ("void %s(void)\n{" % fn) in s and (fn + "_orig") not in s:
            s = s.replace("void %s(void)\n{" % fn, "static void %s_orig(void)\n{" % fn, 1)
            done.append(fn)
    if done:
        if "lambo_unlock_tracks" not in s:
            s += TRACKS_HELPER
        for fn in done:
            s += TRACKS_WRAP % (fn, fn)
        open(p, "w").write(s)
        print("patched unlock-all-tracks", done, "in", p)

# Test switch (RECOMP_TEST_TOD / RECOMP_TEST_WEATHER, numbers as the race
# options menu counts them, 0 first): the level variant lookup
# sub_00107DC0(route, time, weather) -> name gets these instead, when the
# track has that variant (a non-empty name). The race's own copies (time at
# 0x1DC7C4, weather at 0x228258) follow, so its effects match. Logs each
# variant a race loads. For tests only; off unless set.
TESTSW = '''
/* Lamborghini race-variant test switch (post_recomp_bink_fix.py). */
#include <stdio.h>
#include <stdlib.h>
void sub_00107DC0(void)
{
    static int init, tod = -1, wea = -1;
    if (!init) {
        const char *e;
        init = 1;
        if ((e = getenv("RECOMP_TEST_TOD")) && *e) tod = atoi(e);
        if ((e = getenv("RECOMP_TEST_WEATHER")) && *e) wea = atoi(e);
    }
    if (tod >= 0 || wea >= 0) {
        uint32_t r = MEM32(esp + 4), t = MEM32(esp + 8), w = MEM32(esp + 0xC);
        uint32_t nt = tod >= 0 ? (uint32_t)tod : t, nw = wea >= 0 ? (uint32_t)wea : w;
        uint32_t name = r * 0x1108u + ((nw + nt * 4u) << 8) + 0x22E168u;
        int race = t == MEM32(0x1DC7C4) && w == MEM32(0x228258) && r == MEM32(0x228254);
        if (nt < 4 && nw < 4 && MEM8(name)) {
            MEM32(esp + 8) = nt;
            MEM32(esp + 0xC) = nw;
            if (race) {
                MEM32(0x1DC7C4) = nt;
                MEM32(0x228258) = nw;
            }
        }
        if (race) {
            static uint32_t last, listed = 0xFFFFFFFFu;
            if (listed != r) {   /* every variant this route has */
                uint32_t a, b2;
                listed = r;
                for (a = 0; a < 4; a++)
                    for (b2 = 0; b2 < 4; b2++) {
                        uint32_t nm = r * 0x1108u + ((b2 + a * 4u) << 8) + 0x22E168u;
                        if (MEM8(nm))
                            fprintf(stderr, "[RACE]   has time %u weather %u: %.40s\\n", a, b2, (const char *)XBOX_PTR(nm));
                    }
            }
            if (last != name) {
                last = name;
                fprintf(stderr, "[RACE] route %u time %u weather %u: %.40s%s\\n", r, nt, nw,
                        MEM8(name) ? (const char *)XBOX_PTR(name) : "", MEM8(name) ? "" : "(no such variant)");
            }
        }
    }
    sub_00107DC0_orig();
}
'''
for p in glob.glob(os.path.join(gen, "recomp_*.c")):
    s = open(p).read()
    fn = "sub_00107DC0"
    if ("void %s(void)\n{" % fn) in s and (fn + "_orig") not in s:
        s = s.replace("void %s(void)\n{" % fn, "static void %s_orig(void)\n{" % fn, 1)
        s += TESTSW
        open(p, "w").write(s)
        print("patched race-variant test switch in", p)

# Doppler spikes (the "buzz" in races and the intro fly-by): the game hands
# DirectSound 3D velocities worked out as distance moved since last frame
# times 60, so a long frame (a 50-70 ms hitch) makes every car look 3-4x
# faster for one update. DirectSound's Doppler term (sub_0019C084 stores it,
# as pitch units, at +0x38 of the voice's 3D state in esi) then hits the
# +2 octave pitch cap on every 3D voice for a frame or two -- a high chopped
# whine. Limit that term to +/-1 octave and to 1/8 octave of change per
# update; real pass-bys move far slower than that. RECOMP_DOPPLER_LIMIT=0
# turns it off.
DOPPLER = """
/* Lamborghini Doppler limiter (post_recomp_bink_fix.py). */
#include <stdlib.h>
void sub_0019C084(void)
{
    static int on = -1;
    static uint32_t key[512];
    static int32_t last[512];
    uint32_t s = esi;
    sub_0019C084_orig();
    if (on < 0) { const char *e = getenv("RECOMP_DOPPLER_LIMIT"); on = !(e && *e == '0'); }
    if (!on || !s)
        return;
    {
        unsigned h = (s >> 2) * 2654435761u >> 23, n;
        int32_t p = (int32_t)MEM32(s + 0x38), q = p;
        for (n = 0; n < 512 && key[h] && key[h] != s; n++)
            h = (h + 1) & 511;
        if (n == 512)
            return;
        if (key[h] == s) {
            if (q > last[h] + 512) q = last[h] + 512;
            if (q < last[h] - 512) q = last[h] - 512;
        }
        if (q > 4096) q = 4096;
        if (q < -4096) q = -4096;
        key[h] = s;
        last[h] = q;
        if (q != p) {
            MEM32(s + 0x38) = (uint32_t)q;
            MEM32(s + 0x1C) |= 0x10;
        }
    }
}
"""
for p in glob.glob(os.path.join(gen, "recomp_*.c")):
    s = open(p).read()
    fn = "sub_0019C084"
    if ("void %s(void)\n{" % fn) in s and (fn + "_orig") not in s:
        s = s.replace("void %s(void)\n{" % fn, "static void %s_orig(void)\n{" % fn, 1)
        s += DOPPLER
        open(p, "w").write(s)
        print("patched Doppler limiter in", p)
