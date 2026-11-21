# Lamborghini — Xbox static recompilation for Linux

A native Linux port of **Lamborghini**, Rage Software's unreleased original-Xbox racing game
(title ID 52410001, December 2002 pre-release build). It's built by *static recompilation*: the
game's x86 code is translated to C ahead of time with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and compiled into a normal Linux program,
running on an emulated Xbox kernel, GPU (NV2A → OpenGL) and audio (MCPX APU → SDL2).

> **No game files are included, and none are downloadable here.** The repository has only the
> port's own source, the toolkit patch and build scripts. To build it you need **your own
> copy** of the game's files. The recompiled code is generated on your machine from your own
> `default.xbe`.

<!-- Screenshots go here: docs/screenshots/ -->

## What works

- The whole game: intro videos, menus, car select, championship, arcade, free run, time of
  day and weather, and 2–4 player split screen.
- 60 fps in races, locked to the console's 60 Hz like on hardware.
- GPU renderer (OpenGL 4.3) with render scale 1×–4× (or AUTO to match your screen), and true
  widescreen with an optional edge-anchored race HUD.
- Sound: music, engines, effects and 3D positional audio through SDL2.
- Controllers through SDL (hot-plug, multiple pads), plus keyboard.
- Saves in `~/.local/share/xboxrecomp`.

See [Known issues](#known-issues) for what doesn't work yet.

## Requirements

- Linux x86-64 and a GPU with **OpenGL 4.3** (Mesa radeonsi/iris, or NVIDIA).
- Build tools: CMake ≥ 3.20, Ninja, GCC, pkg-config, Python 3 with `capstone`.
- Libraries: SDL2, libepoxy, OpenSSL, FFmpeg (libavcodec/format/util/swscale).

Debian/Ubuntu:
```sh
sudo apt install build-essential cmake ninja-build pkg-config python3 python3-capstone \
    libsdl2-dev libgl1-mesa-dev libepoxy-dev libssl-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
```
Arch: `sudo pacman -S --needed base-devel cmake ninja python-capstone sdl2 mesa libepoxy openssl ffmpeg`
Fedora: `sudo dnf install gcc cmake ninja-build pkgconf python3-capstone SDL2-devel mesa-libGL-devel libepoxy-devel openssl-devel ffmpeg-devel`
(ffmpeg-devel needs RPM Fusion).

## Game files

You need the game's files extracted from the disc into a folder named `Lamborghini/`. That
folder holds `default.xbe` and the `.xfs` archives, plus `Audio/`, `Foreign/` and the rest.

## Building

```sh
git clone https://github.com/trev82usa/Lamborghini-static-recomp
cd Lamborghini-static-recomp
./scripts/build_linux.sh /path/to/Lamborghini/default.xbe
```

The script:
1. clones xboxrecomp next to this repo (`../xboxrecomp`), checks out the tested commit
   (`patches/xboxrecomp-base-commit.txt`) and applies `patches/xboxrecomp-lamborghini.patch`;
2. runs the recompiler on your `default.xbe`, writing the C code to `src/recomp/gen/`
   (git-ignored; never commit or share it);
3. applies the post-recompile fixes (`scripts/post_recomp_bink_fix.py`);
4. builds `build-linux/Lamborghini.x86_64`.

The recompile step takes several minutes. Later rebuilds only need
`ninja -C build-linux Lamborghini`.

## Running

Put `Lamborghini.x86_64` **next to** the `Lamborghini/` data folder and run it:

```
Lamborghini/            <- your game files
Lamborghini.x86_64
```

The log is written to `lamborghini_log.txt` beside the binary.

## Controls

A USB/XInput controller works through SDL and can be plugged in while the game runs. The
keyboard drives pad 1:

| Key | Pad | Key | Pad |
|---|---|---|---|
| Enter | Start | Backspace | Back |
| Z / X / C / V | A / B / X / Y | Q / E | White / Black |
| 1 / 3 | Left / right trigger | Arrows | D-pad |
| W A S D (or keypad 8 4 2 6) | Left stick | I J K L | Right stick |

When racing on the keyboard, **W** accelerates, **S** brakes, **A / D** steer (eased in over
about 0.3 s, so tap for small corrections) and **V** looks back.

| Key | Action |
|---|---|
| **F1** | Settings menu (arrows to choose and change; F1 or Esc closes) |
| **F3** | Frame-rate counter |
| **F11** | Fullscreen |

## Settings (F1)

Saved in `~/.config/xboxrecomp/settings.ini`.

- **Render scale:** AUTO (matches your window/screen height) or 1×–4×. The game is drawn
  at that multiple of 640×480 for a sharp, anti-aliased picture.
- **Widescreen:** FIT SCREEN fills 16:9 / 21:9 screens with a wider field of view. The HUD and
  menus keep their shape in the centre.
- **Widescreen race HUD:** CENTRE, or EDGES to move the race HUD out to the screen edges.
  Menus stay centred.
- **Smooth upscaling**, **fullscreen**, **frame-rate counter**.
- **Unlock all cars / unlock all tracks.**

### Environment switches

| Variable | Effect |
|---|---|
| `RECOMP_GL_SCALE=n` | Force render scale (fractions allowed, e.g. `2.25`) |
| `RECOMP_WIDESCREEN=1`, `RECOMP_HUD_EDGES=1` | Widescreen / edge HUD |
| `RECOMP_WINDOW_SCALE=n` | Starting window size (multiple of 640×480) |
| `RECOMP_FPS_OVERLAY=1` | Start with the frame-rate counter on |
| `RECOMP_KEYBOARD_RACING=0` | Plain keyboard mapping (instant full steering) |
| `RECOMP_GL=0` | CPU software renderer (slow; fallback if OpenGL 4.3 is missing) |
| `RECOMP_FLIP_PACING=0` | Uncap the frame rate (parts of the game then run fast) |

[docs/DEVLOG.md](docs/DEVLOG.md) lists the many debugging and compatibility switches.

## Known issues

- **Night races:** thin light streaks on screen. Headlight pools on the road may not match
  the Xbox.
- **Road:** some dotted/patchy road textures at certain times of day.
- **Sound:** the tyre-skid sound can stutter.
- **Frame hitches:** rare long frames, more often at race start with the whole field in view.
- **Windows:** a MinGW cross-compiled `.exe` builds (software renderer only) but is untested on
  real Windows. Linux is the supported platform.

Bug reports with screenshots or short clips (and `lamborghini_log.txt`) are very welcome.

## Repository layout

| Path | What |
|---|---|
| `src/main.c` | Host entry point: memory map, pushbuffer executor, vblank, window, input, diagnostics |
| `src/audio_shim.c` | SDL2 audio output |
| `src/recomp_manual.c` | Hand-written overrides of recompiled functions |
| `config/seed_functions.json` | Extra function entry points the disassembler misses |
| `patches/xboxrecomp-lamborghini.patch` | All toolkit changes: OpenGL NV2A renderer, APU fixes, Linux kernel/host fixes |
| `scripts/post_recomp_bink_fix.py` | Fixes applied to the generated code after each recompile |
| `scripts/build_linux.sh` | One-step build |
| `docs/DEVLOG.md` | Development notes, build by build |

## Credits and license

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz (MIT): the recompiler and
  runtime this port is built on.
- [xemu](https://xemu.app) was studied as a reference for how the hardware behaves.
- Lamborghini © Rage Software. This project isn't affiliated with or endorsed by the rights
  holders, and includes none of the game's code or assets.

This port's own code and the toolkit patch are released under the [MIT License](LICENSE).
