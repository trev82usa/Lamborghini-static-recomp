# Development log

Notes kept while building the port, newest first, including the many `RECOMP_*` debug and
compatibility switches. Some early entries describe the Windows/Wine build and things that
have changed since. For playing and building, see the [README](../README.md).

## Recent (November 2026)
- **Race audio buzz fixed.** Two causes:
  - The APU left sub-frame holes when the title briefly switched the counter mode off. Voices
    now pause there instead (`RECOMP_APU_XCNTOFF_PAUSE=0` reverts).
  - DirectSound's 3D Doppler term spiked after a frame hitch. The game's velocities are
    distance-per-frame x 60, so a 68 ms frame tripled them and every 3D voice jumped to
    maximum pitch for a frame or two: a chopped 1.3 kHz whine. The Doppler pitch is now
    slew-limited per voice and clamped (`RECOMP_DOPPLER_LIMIT=0` reverts; wrapper in
    `scripts/post_recomp_bink_fix.py`).
  - `RECOMP_APU_PITCHTRAP=1` logs pitch jumps with the guest call stack.
- **Night light flares / mirror cars.** The game tests each light and car with one point.
  At render scales above 1x the pixel count was scaled down to 1x with truncation, so 1 pixel
  read as 0 and every light looked hidden. It now rounds up. Predicted visibility results
  (the default, so the executor never waits for the GPU) are now matched to the right test
  by the point's screen position, not by test order. Order matching handed one car's
  result to another whenever a car came into view (wrong 15% of the time, now 0.3%).
  `RECOMP_GL_REPORT_MATCH=0` reverts.
- **Pause menu in widescreen with EDGES HUD.** The pause menu is drawn over the still-running
  race scene, so it was treated as race HUD and its arc of items torn apart. A frame with
  full-width divider lines (menus only) now keeps the HUD centred.
- Rain drops render as refracting droplets (texture-shader dot-product modes).
- Portable binary: `src/glibc_compat.*` binds fmod/sqrtf/log10f/strtol/strtoul to their old
  glibc symbol versions, so the release runs on glibc 2.34+ instead of only on 2.43
  (`-DLAMBO_PORTABLE_GLIBC=OFF` disables it).
- Missing game files now show a message box explaining where `Lamborghini/default.xbe` goes
  (it used to print a toolkit message about a 'game' folder to the terminal only).

## Playing (Linux, native build)
Put `Lamborghini.x86_64` next to the extracted `Lamborghini/` data folder and run it. It opens
a 1280x960 window (resizable; `RECOMP_WINDOW_SCALE=<n>` changes the start size) and draws on
the GPU (`RECOMP_GL=0` for the software rasteriser). A USB/XInput pad works through SDL and
can be plugged in while the game runs (the log says `[INPUT] controller ... connected`). The
keyboard also drives pad 1:

| Key | Pad | Key | Pad |
|---|---|---|---|
| Enter | Start | Backspace | Back |
| Z / X / C / V | A / B / X / Y | Q / E | White / Black |
| 1 / 3 | Left / right trigger | arrows | D-pad |
| W A S D (or keypad 8 4 2 6) | Left stick | I J K L | Right stick |

**F3** shows a frame-rate counter (frames in the last second, and LOW = slowest frame;
green 60, yellow 30-57, red below 30). `RECOMP_FPS_OVERLAY=1` starts with it on.

**F1** opens the settings menu (arrows to choose and change, F1 or Esc to close; the game gets
no keys while it is open): render scale AUTO or 1x-4x (the GPU draws at that multiple and shrinks the
result for the game, and the window shows the full-resolution frame: sharper and anti-aliased;
2x costs no fps here), smooth upscaling of the window,
fullscreen (also **F11**) and the frame-rate counter. Saved in
`~/.config/xboxrecomp/settings.ini`; Default scale is AUTO; `RECOMP_GL_SCALE=n` overrides it.
AUTO renders at the window's (or screen's) own height and follows resizes: 2.25x on a
1080-line screen, 3x on 1440 (2560x1440 and 3440x1440). Fractional scales also work by hand
(`RECOMP_GL_SCALE=2.25`, `render_scale` in the file is 0 for AUTO). **Widescreen** (F1, or
`widescreen=1` / `RECOMP_WIDESCREEN=1`): FIT SCREEN fills the screen's width (16:9, 21:9, ...).
The game's on-screen cameras get a wider horizontal view (same vertical one) for drawing and
for its culling, the 640x480 frame is drawn anamorphic with extra width at the render scale
(3440x1440 = 3x tall, 5.37x wide) and stretched to the screen. 2D draws (every vertex at
the same small w: HUD, menus, text) are squeezed back by the same factor about the centre, so
they keep their shape in the middle 4:3; single full-width rectangles (backgrounds, fades,
divider bars) stay full width (`RECOMP_WIDE_HUD=0` stretches everything; `RECOMP_HUD_TRACE=s`
lists the draws of the frame s seconds in). A frame that is little more than one
full-screen picture (12 draws or fewer: intro movies, logos, the loading screen) keeps it
4:3 with black side bars; menu backgrounds, drawn under many other draws, fill the width. The
rear-view mirror keeps a 4:3 view and is squeezed with the HUD (draws clipped to a window
narrower than the screen move with it, clip included), so it sits in its frame between the
lap and time boxes. F1 **WIDESCREEN RACE HUD** = EDGES (`hud_edges=1`, `RECOMP_HUD_EDGES=1`)
moves the race HUD to the screen edges: 2D draws (per triangle on the CPU path) wholly in the
left third of the 4:3 layout keep their distance from the left edge, the right third from the
right edge, the rest stays centred; only in frames of 150+ 3D draws (races), menus stay centred.

Driving on the keyboard: **W** accelerates, **S** brakes, **A / D** steer, **V** looks back.
Steering eases in over about 0.3 s, so tap A/D for small corrections and hold for full lock
(`RECOMP_KEYBOARD_RACING=0` gives the plain table above, with instant full lock).
Frames are held to the console's 60 Hz like on hardware (`RECOMP_FLIP_PACING=0` uncaps them,
but parts of the game then run fast). Saves: `~/.local/share/xboxrecomp`. Log: `lamborghini_log.txt` beside the binary.

## Layout
- `src/main.c` - host entry (entry point 0x00129B9D, 128 MB dev-kit RAM, PB exec, vblank, window, keyboard, diagnostics)
- `src/recomp_manual.c` - manual overrides (template)
- `patches/xboxrecomp-lamborghini.patch` - runtime/tool fixes on top of the commit in `xboxrecomp-base-commit.txt`
- `scripts/post_recomp_bink_fix.py` - re-applies the fixes to generated code (Bink pacing, pushbuffer detach, storage-message hold) after regeneration

## Toolkit fixes in the patch
- XBE parser + runtime: pre-release ("beta") entry/thunk XOR keys
- 128 MB contiguous window (dev-kit build), pinned physical allocs fail like hardware
- File I/O: full host sharing, no FILE_FLAG_NO_BUFFERING, reads clamped to EOF
- Handle tokens with bit 30 cleared (XAPI flag) resolve correctly
- Semaphore-target resolver hook for the D3D fence

## Build (cross-compile from Linux, MinGW)
```
git clone https://github.com/sp00nznet/xboxrecomp && cd xboxrecomp
git checkout $(cat ../Lamborghini-Recomp/patches/xboxrecomp-base-commit.txt)
git apply ../Lamborghini-Recomp/patches/xboxrecomp-lamborghini.patch
# pipeline (disasm WITHOUT --text-only so XDK sections are included)
python3 -m tools.xbe_parser game/default.xbe --json game/default_analysis.json
python3 -m tools.disasm game/default.xbe -v --seed-functions ../Lamborghini-Recomp/config/seed_functions.json
python3 -m tools.func_id game/default.xbe -v
python3 -m tools.abi_analysis game/default.xbe -v
python3 -m tools.recomp game/default.xbe --all --split 250 --gen-dir ../Lamborghini-Recomp/src/recomp/gen
cd ../Lamborghini-Recomp && python3 scripts/post_recomp_bink_fix.py src/recomp/gen
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=../xboxrecomp/cmake/mingw-w64-x86_64.cmake -DXBOXRECOMP_DIR=../xboxrecomp
ninja -C build -j1 Lamborghini.exe
```
Put `Lamborghini.exe` next to the extracted `Lamborghini/` data folder.

## Status (build 31) - championship playable, GPU renderer fixes
Championship mode works through car select, the race list and races (Seoul, Black Forest
checked against Xbox footage). Race 60 fps on Black Forest, ~40 at the start with the whole
field in view; controller hot-plug; frames held to 60 Hz (`RECOMP_FLIP_PACING`); keyboard
driving W/S throttle/brake with eased A/D steering (`RECOMP_KEYBOARD_RACING`).

Fixes:
- F1 menu **UNLOCK ALL CARS** / **UNLOCK ALL TRACKS** (`unlock_cars=1` / `unlock_tracks=1` in
  settings.ini, `RECOMP_UNLOCK_CARS=1` / `RECOMP_UNLOCK_TRACKS=1`). Tracks: the arcade track and
  route readers (sub_000A03B0..04E0) first run the game's own unlock (sub_000A0560, the DR CUNNING
  name cheat, also what a won championship calls), once and again whenever the game rewrites the
  lock table. All ten tracks have data on the disc. The profile saves both unlocks. Night is
  offered in Free Run only, unlock or not.
- GPU renderer: line primitives (D3D line lists/strips, used for thin effects such as rain
  streaks) were dropped on every path; vertex-program lines are now drawn, widened with the
  render scale so they keep their thickness (`RECOMP_GL_LINE_WIDTH=<w>` sets it in guest
  pixels). `[GPU] line batches:` in the log counts them, including any still not drawn
  (fixed-function / CPU fallback).
- GPU renderer and software path: the texture-shader dot-product modes (DOT_ST, DOT_ZW,
  DOT_PRODUCT, DOT_RFLCT_DIFF/SPEC/SPEC_CONST, DOT_STR_3D/CUBE) and dependent reads
  (DEPENDENT_AR/GB) used to sample black. They drive bumped reflections, water and wet surfaces
  and per-pixel lighting, and now follow xemu, with SET_DOT_RGBMAPPING and SET_EYE_VECTOR. Not
  modelled: BRDF, DOT_ZW's depth replace, true 3D textures and exact HILO formats.
  `[GPU] texture shader mode N first used` logs each mode the first time the game uses it.
- No 3 s freeze on "Loading <profile>. Please don't turn off your Xbox console": the game
  busy-waited (no frames drawn) until storage messages had been up their minimum time, after
  the load/save had already finished. `scripts/post_recomp_bink_fix.py` now skips that hold
  in sub_0003C150 (both copies); `RECOMP_SAVE_HOLD=1` keeps it.
- Race-start stutter gone (AMD): radeonsi compiled "optimized" shader variants in the background
  and swapped them in during the countdown, stalling the GPU 100-300 ms each time (GPU timestamps:
  GPU idle, ~30 MB allocated per swap). The game now sets `AMD_DEBUG=nooptvariant` unless
  `AMD_DEBUG` is already set; Black Forest start 55-58 -> 59-60 fps, no frames over 100 ms.
- Back-face culling in the GPU renderer: the title's SET_CULL_FACE_ENABLE / CULL_FACE /
  FRONT_FACE were ignored, so faces meant to be hidden (e.g. the inside of the Jalpa's front
  bumper, seen as a thin yellow line) were drawn. `RECOMP_GL_CULL=0` draws everything as before.
- Smoother motion: when the game queues two frames in one pushbuffer segment, the second
  flip now waits for its own vblank inside the walk (GET published to the walk position
  first, so D3D's space-wait marker still works). Before, both reached the screen in one
  vblank, so races juddered at 60 fps; now one frame per vblank (`RECOMP_FLIP_INLINE=0` off).
- Render scale 2x-4x now reaches the screen: the window gets the whole scaled frame (e.g.
  1280x960 at 2x) instead of a 640x480 copy (`RECOMP_GL_HIRES=0` for the old behaviour).
- DXT textures go to the GPU compressed with the game's own mip levels instead of being
  decoded texel by texel on the executor (2-3 ms each; most of the race-start stall). A
  texture whose stored mips don't match its top level (the car paint, built each race) is
  still decoded with GPU-made mips. `RECOMP_GL_DXT=0` decodes everything as before.
- Uncompressed textures (32/16/8-bit, paletted; swizzled or linear) decode a row at a time
  instead of one texel per call: ~10x faster, menus and video frames included
  (`RECOMP_GL_FAST_DECODE=0` old path, `=2` checks every decode against it).
- Diagnostics: `RECOMP_FRAME_LOG=1` (frames over 25 ms), `RECOMP_WAIT_PROFILE=1` (time the game
  blocks per call site), `[PB] slow walk` lines for executor walks over 50 ms.
- Multiplayer: the emulated USB bus now plugs in one pad per connected controller (hot-plug
  too; `RECOMP_USB_PADS=auto`, the default), so two/three/four player unlock. Checked 2P
  (60 fps) and 4P (30 fps: the game itself asks for every other vblank in four-way split).
- F1 settings menu; GPU render scale 1x-4x (`RECOMP_GL_SCALE`), switchable live: targets are
  written back at the old scale and rebuilt at the new one at the next flip.
- `RECOMP_STALL_DIAG=1` prints, when the title stops submitting, where PUT, GET and D3D's write
  pointer stand, the unsubmitted words and the visibility slots still pending.
- Visibility tests never wait for the GPU: answered at once with the same test's result from
  the previous frame, the real count used next frame (`RECOMP_GL_ASYNC_REPORTS=0` synchronous,
  `=1` asynchronous). Removed about 6 ms of executor stall per frame.
- Frame pacing: a held flip no longer withholds the PFB flush ack, so the title keeps building
  its next frame while the current one waits for the vblank (slowest race stretch 47 -> 55 fps).
  `RECOMP_FLIP_HOLD_FLUSH=0` restores the old behaviour. Per-draw timers only with
  `RECOMP_GPU_TIMING=1`; the profiler (`LAMBO_PROFILE=1`) now works during races.
- GPU renderer: a render target is not written back when guest memory changed after the GPU
  took it (the title reused the memory). Lamborghini loads the challenge car's vertices into a
  freed paint target; the stale write-back zeroed them (car as four wheels, then a divide-by-zero
  crash in overtake). `RECOMP_GL_RB_GUARD=0` restores the old behaviour.
- Crash handler also reports SIGFPE/SIGILL (an integer divide by zero used to exit silently).
- Vertex programs on the GPU (Linux): the NV2A vertex program runs in a GLSL interpreter
  (`VS_VSH` in nv2a_gl.c, fed by `nv2a_vsh_export`), and the title's vertex bytes go to GL as
  they are (short/byte/float/D3DCOLOR formats; the 11:11:10 normal decoded in the shader).
  The executor's work in a busy race roughly halves. Programs that write c[] and point batches
  (visibility tests) stay on the CPU. `RECOMP_GPU_VSH=0` transforms everything on the CPU,
  `RECOMP_GPU_VSH_RAW=0` gathers vertices on the CPU but still transforms on the GPU.
- Texture change checks: sampled every frame, full hash staggered over 8 frames
  (`RECOMP_GL_SPARSE_TEX=0` = full hash every frame).
- In-car view: window clipping (`SET_WINDOW_CLIP_*`) honoured. The rear-view mirror is the scene
  drawn again inside a small window clip; ignoring it painted the mirror's backward view over the
  whole screen once the race started (`RECOMP_WINDOW_CLIP=0` ignores it).
- Intro and menu videos play smoothly: Bink paces itself again (the videos are 25 fps
  interlaced, 50 field updates a second); the old forced 25 fps wrapper throttled them into
  stop-start bursts (`RECOMP_BINK_PACE=1` restores it).
- ChallengeCallback / OnlineSearchCallback / GameCallback seeded (cash challenge after round 3
  ignored A); all 207 registered screen handlers now have code.
- MainChampCallback (0xD41A0) was never recompiled (only referenced by pointer), so the
  championship screen ignored presses: seeded in `config/seed_functions.json`, code regenerated.
- D3D's pushbuffer-space wait hung on an event it builds itself: title-built KEVENTs are on, and
  software method 5 now sets that event (`lambo_sw_method` in main.c).
- Linux file lookups ignore case (music `TRACK24.wax` vs `track24.wax`).
- GPU renderer: stencil test/ops/clear, and bump-env texture stages (software renderer too).
  Seoul's wet-road reflection is a full-screen pass limited to the road by stencil; without
  stencil it painted over the whole track.
- GPU renderer: render targets the title only draws and then leaves (never presented or sampled
  by the GPU) are written back to guest memory when the GPU catches up with PUT. The game draws
  each car's paint into one and compresses it into the car texture on the CPU; without this the
  cars were flat yellow (`RECOMP_GL_SYNC_ON_DRAIN=0/2` off/everything).
- GPU renderer: textures are fully re-hashed every frame they are used (stale menu/loading
  screen textures); `RECOMP_GL_SPARSE_TEX=1` restores the cheaper sampled check.
- GPU renderer: depth goes straight into [0,1] (`glClipControl`); the [-1,1] mapping lost the
  precision a whole track needs near the far plane and Black Forest's trees showed through walls
  (`RECOMP_GL_Z01=0` for the old mapping).
- GPU renderer: textures with mip levels get a GPU-generated mip chain and the title's mip
  filter, so distant trees, fences and road no longer sparkle (`RECOMP_GL_MIPMAPS=0` off).
- GPU renderer: a pass that samples the surface it draws into reads a GPU copy of it instead of
  a readback + re-decode (not yet seen to trigger in testing).
- Car paint targets are written back again once the GPU has not used them for 8 frames, so the
  paint built before race 2 reaches the car (it was flat yellow from the second race on).

## Status (build 30) - pixels on the host GPU (Linux)
The native Linux build draws on the GPU: race ~50 fps (stuck against a barrier ~75), menus
~150, against ~5.4 / ~41 on the software rasteriser. Renders match the software path (race,
HUD, menus, car select, cube-map reflections). On by default in `Lamborghini.x86_64`;
`RECOMP_GL=0` goes back to the software rasteriser, which is also the automatic fallback when
no EGL / OpenGL 4.3 context is available. The Windows exe is unchanged (software), apart from
the faster vertex stage below.
- `xboxrecomp/src/kernel/nv2a_gl.c` (new, ours; xemu not used): vertex processing stays on
  the CPU; the executor's triangle queue, visibility points, clears and reports go to OpenGL
  4.3 on a surfaceless EGL context owned by the executor thread.
  - One fragment shader interprets the register-combiner words from uniforms (a GLSL
    transcription of `nv2a_combiner.c`), with all four texture stages (2D, cube, passthru,
    clip-plane), fog, alpha test; blend/depth/colour-mask map to GL state. Colours and fog
    interpolate in screen space, texture coordinates with perspective, as the CPU path does.
  - Textures decode through the executor's own sampler (every format it knows) and are cached
    by address with a sparse per-frame / full every-30-frames content check.
  - Render targets stay on the GPU: a texture that is a surface (same memory, layout and a
    compatible format) is sampled in place; a cube map whose faces are render targets is
    assembled with glCopyImageSubData. A surface goes back to guest memory only when the CPU
    side needs it (an overlapping surface or texture decode, eviction, snapshots).
  - Lamborghini clears its cube faces as linear surfaces and draws them swizzled; the same
    memory and size in the other layout reuses the GPU copy.
  - The flip reads the frame into a pixel buffer with a fence and hands the window the
    previous one (one frame of latency, no GPU stall).
  - Visibility tests use GL occlusion queries.
  - Counters in the 10 s report: `[GL] ...` lines.
- Main menu fixed (both builds): it clears alpha alone mid-frame (CLEAR_SURFACE 0x83: Z,
  stencil, A) to mask the logo effect; clears treated any colour bit as all four channels and
  wiped the menu items. Clears now write only the named channels (R 0x10 G 0x20 B 0x40 A 0x80).
- RECOMP_FRAME_TRACE also dumps the traced frame's surface at its end (RECOMP_FB_DUMP).
- Vertex stage (both builds): each distinct index of a batch is transformed once, big
  vertex-program batches are split across the raster pool's threads, and the program is
  decoded once per upload instead of per vertex (race ~55 fps).
- Next: vertex programs on the GPU (the executor is now ~50% vertex programs), then trim the
  per-draw work.

## Status (build 29) - native Linux build
`Lamborghini.x86_64` runs natively on Linux (SDL2 window, no Wine): intro, menus (~41 fps),
car select and race (~5.4 fps, Wine/clang 5.8) render correctly and the pad works.
The Windows exe still builds and behaves as before.
- Build (gcc; deps: `xboxrecomp/tools/linux/install_deps.sh`):
  ```
  cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
  ninja -C build-linux Lamborghini
  ```
  Put `Lamborghini.x86_64` next to the `Lamborghini/` data folder. Saves live in
  `~/.local/share/xboxrecomp` (copy a Wine save from
  `~/.wine/drive_c/users/<you>/AppData/Local/xboxrecomp` to keep your profile).
- Toolkit fixes for x86-64 Linux (in the patch):
  - Trapped hardware registers (APU, AC'97, USB OHCI) had a decoder only on Windows and
    arm64, so the pad and sound were off. The SIGSEGV handler now copies the ucontext into a
    Win32-shaped CONTEXT and reuses the Windows x86-64 decoder and device hooks.
  - 128 MB dev-kit RAM: the RAM-mirror reservation ended exactly at guest 0x80000000, and with
    Linux's top-down placement the contiguous window there was already taken (crash on the
    first kernel-page write). POSIX now holds the whole 4 GB guest window in that case.
  - POSIX handles: a guest address passed through as an "untagged" handle was dereferenced
    (SIGSEGV in pthread_mutex_lock from KeWaitForSingleObject). Handles are now checked
    against a live-object registry and fail like Win32 (WAIT_FAILED / ERROR_INVALID_HANDLE).
  - The rasteriser's worker pool was Win32-only, so Linux drew every pixel on one thread.
  - The `[GPU] fps` line used clock(), which is CPU time on Linux (reported ~1/10 the real rate).
  - The SDL window writes finished-frame snapshots (`<RECOMP_FB_DUMP>win00000.bmp`,
    every `RECOMP_FB_WINDOW_DUMP_EVERY` shown frames). The executor's own dump reads the
    surface being drawn and can catch half a frame.
- Game side: `main.c` has POSIX versions of the crash handler (sigaction + dladdr names the
  guest function), LAMBO_PROFILE (per-thread wall-clock sampler using tgkill/SIGPROF) and the
  start-up code; on Linux the game boots through `xbox_HostWindowMain` so SDL owns the main
  thread.

## Status (build 28)
Menus -> car select -> race all work under Wine (clang/llvm-mingw build, `build-clang`).
Race runs at ~5 fps on the CPU software renderer (pixel work ~80% of executor time, vertex
programs ~15%), so CPU tuning alone won't reach 30 fps.
- Visibility-test points with negative w are now handled. Opponent cars still vanish in the
  rear view (hold Y): the few negative-w points seen had z beyond SET_CLIP_MAX, so the cause
  is probably elsewhere.
- Known: main menu shows only the logo and B icon (fixed in build 30).
- Next: native Linux build (SDL2 + OpenGL), then move pixel drawing to the GPU (register
  combiners -> GLSL), then vertex programs -> GLSL. The Wine build stays supported.

## Status (build 27)
- Visibility tests now work. The game tests each opponent car / the sun by drawing ONE inline
  point (vertex format 0x1760=0x32, BEGIN POINTS, INLINE_ARRAY 0,0,0, END) between
  CLEAR_REPORT_VALUE and GET_REPORT, through the fixed-function transform. Points were never
  rasterised (inline batches needed >= 3 vertices, batch_is_ffp excluded inline batches, no
  POINTS case), so every report counted 0 pixels: the likely cause of opponents vanishing once
  the race starts. Added raster_xf_point (depth test + count, one pixel). In a race about 1 in 6
  points now passes; the rest are behind the camera or off-screen.
- Queued triangles are flushed before CLEAR_REPORT_VALUE / ZPASS enable / GET_REPORT.
- RECOMP_REPORT_TRACE=1 logs each visibility test (draw commands between clear and read).
- Not yet verified by driving: whether opponents now stay visible (needs a human at the pad).

## Status (build 26)
Built with clang (llvm-mingw) instead of GCC: about twice as fast.
Menus ~40 fps (were ~25), race ~5.7-6 fps (was ~2.9), rendering unchanged.
- Why: GCC's MinGW target only has emulated TLS, so every guest register access
  (RECOMP_TLS) and every NV_TLS access in the rasteriser called __emutls_get_address.
  clang emits native Windows TLS.
- Build (toolchain: llvm-mingw msvcrt release, e.g. 20261020, extracted next to the repo):
  ```
  cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake \
    -DLLVM_MINGW_DIR=../toolchains/llvm-mingw-20261020-msvcrt-ubuntu-22.04-x86_64 \
    -DXBOXRECOMP_DIR=../xboxrecomp
  ninja -C build-clang Lamborghini.exe
  ```
  A full clang build takes under a minute (GCC took many).
- `-fno-strict-aliasing -fwrapv` are now set for everything: recompiled code reads guest memory
  through casted pointers and relies on wrap-around. The first clang test run (before these
  flags) froze once in the menus around loading the save (main thread polling
  KeWaitForSingleObject); not seen again in later runs, cause not confirmed.

## Status (build 25)
- Fixed the window freezing on a menu transition (crest screen with a strip of the old menu):
  the window showed the surface drawn last, and the game draws transitions into a 640x480
  off-screen texture at 0x01000000. The executor now presents this frame's back buffer (the
  colour offset D3D's present sets right after FLIP_STALL) once it has been drawn into.
- Speed findings (race ~3 fps): the executor thread is saturated; ~80% of it is the raster
  pool, ~15% vertex programs. The pool's 12 workers are genuinely busy (~0.5 us/pixel/core,
  ~8.5M bbox pixels per frame: the race draws the scene several times per frame). Spinning
  hand-off and executor-dispatch-only were tried and gave nothing; reverted.
  Every NV_TLS access (DXT cache, combiner constants) and every guest register access goes
  through MinGW's emutls (__emutls_get_address). Next: clang/llvm-mingw for native TLS, then
  per-pixel work (early depth test, fewer TLS lookups).
- The report now prints executor time (walk / vertex programs / raster) and raster runs.
- Early PFB-flush acks inside the segment walk make D3D take its "GPU far behind" path and
  wait on a software-method interrupt that is never delivered (hang). Don't.
- Main menu items are not drawn (the logo screen); selecting still works.

## Status (build 24)
Car select and the race now render correctly (car in one piece and shaded, race scene with no
green tint or black wedges). Race runs at ~3.5 fps, car select ~8 fps (CPU renderer, one thread).
- Cause (toolkit, `nv2a_pb_scan.c`): XDK D3D's RunPushBuffer. A NOP software method, a CALL,
  then `0, callee|0x80000000, length, return` (sometimes one padding word). The recorded
  pushbuffer has no RETURN; on the console D3D's software-method interrupt brings the GPU back
  to the return address after `length` bytes. The walk ran off the callee's end into
  unrelated memory and then executed the bookkeeping as commands, desynchronising the rest of
  the frame (garbage surface/blend/transform state: scattered car, green tint, black shapes).
  The walk now bounds the callee by `length` and resumes at the return address.
- RECOMP_PB_DESYNC_LOG=1 logs the first words the walk cannot decode, with raw context.
- RECOMP_FRAME_TRACE also prints each vertex-program batch's inputs (layout and first vertex).
- LAMBO_PROFILE=1: in-process sampling profiler, hottest host addresses every 10 s (resolve
  with `nm -n`).

## Status (build 23)
Menus -> car select -> transmission -> track load -> race intro ("Welcome to Black Forest") now runs.
- Freeze after car select fixed (toolkit, `nv2a_pb_scan.c`): the segment scan stopped at the
  ring's wrap JUMP and guessed the rest from the highest/lowest PUT ever seen, dropping the
  commands either side of the real JUMP. A lost GET_REPORT left the game spinning in
  GetVisibilityTestResult (D3DERR_TESTINCOMPLETE, loop at 0x18210, lens-flare test in
  0xC6B10). After 25 s the game's own watchdog thread (0xAF3A0) saw no progress and showed
  "There is a problem with the disc". `nv2a_pb_scan_follow` now follows the JUMP to PUT.
- New threads inherited the creator's IRQL (`xbox_AllocThreadTib` copies the main TIB,
  KPCR.Irql at +0x24 included), so the XDK CRT's _getptd (0x12EAB3) called
  KeBugCheck(IRQL_NOT_LESS_OR_EQUAL). Reset to PASSIVE.
- After A on car select an automatic/manual popup appears (sometimes drawn with no text).
  At ~3 fps quick taps are missed; hold A.
- LAMBO_STACK_SAMPLE=<ms> prints the guest call chain, pad word, D3D visibility-test
  records, PUT/GET and the last GET_REPORT.
- Known: with sound off (LAMBO_NO_SOUND / "nosound" exe) the race crashes on a divide by
  zero in DirectSound (0x19E7CD, voice count 0). In-race rendering has green tint and
  broken polygons; car model vertex formats (S32K positions, packed normals) look misread.

## Status (build 22)
Boots, plays the Bink intro, the Lamborghini splash and the attract demo at 25 fps.
- Build 22 input fix: the OHCI model's bus_resolve sent every physical address below the
  contiguous high-water mark to the 0x80000000 window. XAPI's XID driver points the pad's
  interrupt transfers at a buffer inside the device object, which is heap memory, so every
  report landed in a D3D buffer (the coloured dots on the speedo) and the pad state never
  changed. Pages the title named via MmGetPhysicalAddress (heap) now resolve to RAM. Locally
  A and Start now reach the game's pad table (0x358050).
- Build 22 crash fix: at the end of the attract demo something zeroes the main TIB's fs:[4],
  and the next SetLastError faulted at 0xFFFFFFEC. The kernel dispatcher now watches fs:[4]
  ([TIBWATCH]) and puts it back. Locally the run now carries on past that point.
- Build 22 sound: RECOMP_AC97_READY=1 by default (emulated APU, XAudio2 output). Untested on
  real Windows (Wine in the container has no audio device). An exe whose name contains
  "nosound" runs silent.
- LAMBO_PAD_PEEK=1 logs the game's pad table whenever it changes.
- Watchdog, window snapshots and the GPU sampler are opt-in (RECOMP_WATCHDOG_SECS,
  RECOMP_FB_DUMP / RECOMP_FB_WINDOW_DUMP_EVERY, LAMBO_GPU_SAMPLER).

## Next steps
1. Confirm Start/A skip the cutscene and reach the menus; find out who zeroes fs:[4]
   (TIBWATCH names the first kernel call that sees it).
2. Local test rig: Wine + Xvfb in the session; scripted input via
   RECOMP_PAD_SCRIPT="12000:START,14000:A". Stage the rest of the game data for menus/tracks.
3. Then: confirm audio on real Windows, and in-race rendering.
