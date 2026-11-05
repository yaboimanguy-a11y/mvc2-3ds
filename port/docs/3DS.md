# Marvel vs. Capcom 2 on the Nintendo 3DS (v0.1)

> **New Nintendo 3DS / New 3DS XL / New 2DS XL only.** The original 3DS,
> 3DS XL and 2DS are too slow: the port needs the New models' 804 MHz CPU
> mode, L2 cache and extra memory.
>
> **No game files are included.** You need your own dump of the PS2 disc
> (NTSC-U, `SLUS-20486`). The 3DS app reads all of the game's data, and the
> few bytes of program data the decompilation doesn't cover, from your disc
> image when it starts.

This is a native port. The game's own decompiled C code is compiled for the
3DS's ARM11 CPU and runs directly, with no emulator. Only the PS2 hardware
layer is replaced: graphics (citro3d on the 3DS GPU), sound (the 3DS DSP),
controls, saves and disc access.

- [Installing and playing](#installing-and-playing)
- [Controls](#controls)
- [The bottom screen](#the-bottom-screen)
- [Troubleshooting](#troubleshooting)
- [Known issues in v0.1](#known-issues-in-v01)
- [Building it yourself](#building-it-yourself)
- [How the port works](#how-the-port-works)
- [Credits and licenses](#credits-and-licenses)

---

## Installing and playing

### What you need

| | |
| --- | --- |
| Console | A **New** Nintendo 3DS, New 3DS XL or New 2DS XL |
| Firmware | Custom firmware ([Luma3DS](https://github.com/LumaTeam/Luma3DS)) with the **Homebrew Launcher**. The usual setup guide is [3ds.hacks.guide](https://3ds.hacks.guide/). |
| SD card | About 2–4 GB free for the disc image |
| Game | Your own dump of *Marvel vs. Capcom 2: New Age of Heroes*, PS2, NTSC-U (`SLUS-20486`), as `.iso`, `.bin` + `.cue`, or `.bin` alone |
| For sound | `sdmc:/3ds/dspfirm.cdc` (see step 1) |

### Step by step

1. **Sound firmware (one time only).** The 3DS's sound chip needs a
   firmware file that Nintendo doesn't let homebrew ship. Run the
   [DSP1](https://github.com/zoogie/DSP1/releases) homebrew once and it writes
   `sdmc:/3ds/dspfirm.cdc`. If you already play other homebrew with sound,
   you already have it. Without it the game still runs, just silently.

2. **Download the release.** Get `mvc2-3ds-v0.1.zip` from the
   [Releases](../../../releases) page and unzip it. Inside is a folder `3ds/mvc2/`:

   ```
   3ds/mvc2/mvc2.3dsx            the game
   3ds/mvc2/mvc2_rtest.3dsx      renderer self-test (no disc needed)
   3ds/mvc2/mvc2_boottest.3dsx   speed test of the game logic, no picture
   3ds/mvc2/movelist.txt         the bottom screen's move list
   3ds/mvc2/README.txt           short version of this guide
   ```

3. **Add your disc image.** Put your disc image in that same `mvc2` folder.
   Any file name works:

   ```
   3ds/mvc2/Marvel vs. Capcom 2 (USA).iso
   ```
   or
   ```
   3ds/mvc2/Marvel vs. Capcom 2 (USA).bin
   3ds/mvc2/Marvel vs. Capcom 2 (USA).cue
   ```

   The app looks for a `.iso` first, then a `.cue` (and the `.bin` it
   names), then any `.bin`. A `.cue` that names a `.bin` with a different
   file name still works if a `.bin` with the cue's own name is next to it.
   Raw 2352-byte and Mode 2 images are both read directly, so you don't
   need to convert anything.

4. **Copy to the SD card.** Turn the 3DS off, put the SD card in your
   computer, and copy the `3ds` folder to the root of the card so that
   you end up with `SD:/3ds/mvc2/mvc2.3dsx` next to your disc image.

   > **SD card format:** a FAT32 card can't hold files over 4 GB. The MvC2
   > disc image is smaller than that, so FAT32 is fine.

5. **Play.** Put the card back, boot the 3DS while holding nothing (or
   however you normally reach Luma), open the **Homebrew Launcher** and
   start **Marvel vs. Capcom 2**. The first boot shows a text log on the
   bottom screen for a few seconds while it opens the disc. Then the game
   takes over the top screen and the move list appears on the bottom one.

   Saves go to `sdmc:/3ds/mvc2/` and work like the PS2 memory card. The
   game's own "No save data found" message on first boot is normal.

---

## Controls

| 3DS | PS2 | In MvC2 (default) |
| --- | --- | --- |
| D-pad / Circle Pad | D-pad / left stick | Move |
| **Y** | Square | Light punch |
| **X** | Triangle | Heavy punch |
| **B** | Cross | Light kick |
| **A** | Circle | Heavy kick |
| **L** | L1 | Assist 1 |
| **R** | R1 | Assist 2 |
| **ZL / ZR** | L2 / R2 | (as set in the game's options) |
| START / SELECT | START / SELECT | |
| **L + R + START + SELECT** | | Quit to the Homebrew Launcher |

You can remap buttons in the game's own options menu. The move list always
shows the default layout above.

**Hold a button while the app starts** (troubleshooting only):

| Held at start | Effect |
| --- | --- |
| L | 3D models (stage backgrounds and effects) off. For isolating a rendering problem. |
| R | Leave the CPU in standard float rounding instead of the PS2-style rounding. For isolating a desync or a logic problem. |

---

## The bottom screen

The bottom screen shows the **move list for player 1's point character**
and follows it when you tag in someone else. Inputs are drawn as what to
press: arrows for motions and coloured badges for the face buttons
(**Y** light punch, **X** heavy punch, **B** light kick, **A** heavy kick,
**L**/**R** assists). Gold entries are hyper combos.

Touch controls:

| Touch | Does |
| --- | --- |
| Name row, left or right half | Previous / next member of your team |
| The list | Next page |
| Bottom row, left | Frame skip on / off |
| Bottom row, right | 4:3 or stretched picture |

The list comes from `movelist.txt` in the app folder, a plain text file
you can edit. The format is described at the top of the file.

---

## Troubleshooting

The game writes **`sdmc:/3ds/mvc2/mvc2_log.txt`** on every run, replacing
the previous one. If something goes wrong, that file says how far it got.

| Problem | What to try |
| --- | --- |
| "No disc image found" | The image must be in the same folder as `mvc2.3dsx`, with a `.iso`, `.bin` or `.cue` extension. |
| "Not the right disc" / immediate exit | It must be the **NTSC-U** disc `SLUS-20486`. PAL and Japanese discs have different code. |
| No sound | Check that `sdmc:/3ds/dspfirm.cdc` exists (step 1). The log shows which CPU core the sound runs on. |
| Runs slowly or crashes during start-up on an original 3DS | Not supported, see the top of this page. |
| Graphics look wrong | Run **MvC2 renderer test** (`mvc2_rtest.3dsx`). It needs no disc, checks the GPU output itself and writes `mvc2_rtest_log.txt`. |
| Console crashes (Luma exception screen) | Luma saves a dump to `sdmc:/luma/dumps/arm11/`. Open an issue with that `.dmp` file and `mvc2_log.txt`. |

---

## Known issues in v0.1

This is a first release. It plays full fights, but expect rough edges:

- **Speed.** Busy fights with three characters, assists and hyper effects
  can need more than one frame of CPU time. Frame skip (on by default)
  keeps the game's speed right by skipping drawing, so the picture can
  look less smooth in those moments. Turn it off from the bottom screen to
  see every frame at a lower speed.
- **3D models are transformed on the CPU.** The 3DS GPU's vertex-shader
  path for the game's 3D stages hung the console in testing, so models
  are transformed on the CPU and drawn through the 2D path. This is
  slower, and textured 3D models are drawn without fog.
- **Hardware testing is limited.** The latest fixes (start-up crash fix,
  bottom-screen blending, sound on a separate CPU core) have had less
  testing on real hardware than the rest.
- **Not yet done:** stereoscopic 3D, Old 3DS support, CIA (home menu)
  install, button remapping for the move list.

---

## Building it yourself

The release `.3dsx` files are built exactly like this. Everything below runs
on macOS or Linux. On Windows, use WSL or MSYS2.

### 1. Tools

| Tool | Why | Install |
| --- | --- | --- |
| **devkitPro**, `3ds-dev` group | devkitARM (linker, libc), libctru, citro3d, citro2d, `picasso` (shader assembler), `3dsxtool`, `smdhtool` | Installer from [devkitpro.org](https://devkitpro.org/wiki/Getting_Started), then `sudo dkp-pacman -S 3ds-dev` |
| **LLVM clang** with the ARM backend | Compiles the game's C. Apple's built-in clang lacks `llvm-ar`. | macOS: `brew install llvm` · Linux: `clang` + `llvm` packages |
| **CMake** 3.20+ and **Ninja** | Build system | `brew install cmake ninja` / `apt install cmake ninja-build` |
| **Python 3** | The port's code generators | Usually already installed |
| **Emscripten** | Only for a build step that scans the decompiled code's function signatures. It runs a WebAssembly compile, never a 3DS one. | macOS: `brew install emscripten` · or an [emsdk](https://emscripten.org/docs/getting_started/downloads.html) checkout with `EMSDK` set |

The versions used for v0.1: devkitARM r68, libctru 2.7.1, citro3d 1.7.1,
citro2d 1.7.0, picasso 2.7.2, Homebrew clang 22.1, Emscripten 6.0, CMake 4.4,
Ninja 1.13, Python 3.

The build looks for devkitPro in `$DEVKITPRO`, or `/opt/devkitpro` if
that's not set. It looks for clang in Homebrew's LLVM
(`/opt/homebrew/opt/llvm/bin` or `/usr/local/opt/llvm/bin`), then
`$LLVM_ROOT`, then your `PATH`.

### 2. Build

```bash
git clone https://github.com/yaboimanguy-a11y/mvc2-3ds.git
cd mvc2-3ds
cmake -S port -B build/port-3ds -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/port/cmake/3ds-clang.cmake
ninja -C build/port-3ds mvc2_3ds_sdcard
```

The first build step scans the decompiled sources for function signatures
with Emscripten, which takes a couple of minutes. The build finds a
Homebrew Emscripten by itself; for an emsdk checkout, set `EMSDK`. To skip
the scan on a later build directory, reuse its table with
`-DMVC2_HOST_SIGS=build/port-3ds/host_sigs.json`. The first full build compiles about 20,000
decompiled functions and takes roughly 10–20 minutes. The result is a
ready-to-copy SD card folder:

```
build/port-3ds/sdcard/3ds/mvc2/   mvc2.3dsx, mvc2_rtest.3dsx, mvc2_boottest.3dsx,
                                  movelist.txt, README.txt
```

Add your disc image to that folder and copy it to the SD card as in
[step 4](#step-by-step). No disc image is needed to *build*.

Individual targets: `mvc2_3ds` (the game), `mvc2_rtest` (renderer test),
`mvc2_host_3dsx` (boot test).

### 3. Sending builds over Wi-Fi (optional)

Press **Y** in the Homebrew Launcher to wait for a network load, then:

```bash
/opt/devkitpro/tools/bin/3dslink build/port-3ds/mvc2.3dsx
```

The disc image still has to be in `sdmc:/3ds/mvc2/`.

### 4. Reading crash dumps

`port/tools/luma_dump.py crash_dump_XXXX.dmp` decodes a Luma ARM11
exception dump: the exception type and the registers. Look up `pc` and `lr`
in the matching ELF with
`llvm-addr2line -fe build/port-3ds/mvc2.elf 0x<pc>`. Keep the `.elf` that
goes with each `.3dsx` you test.

---

## How the port works

The upstream project (see the main [README](../../README.md)) is a
byte-matching decompilation of the PS2 executable plus a port layer that
compiles that C for a modern 32-bit host. It already ran in the browser
(WebAssembly). The 3DS port adds a third target to the same pipeline.

### Toolchain (`port/cmake/3ds-clang.cmake`)

- **clang** compiles everything for `armv6k-none-eabihf`
  (`-mcpu=mpcore -mfpu=vfpv2 -mfloat-abi=hard -marm`). The link goes
  through **devkitARM's gcc** with `3dsx.specs`, so crt0, newlib and
  libctru are devkitPro's own.
- **Enum size:** devkitPro's libraries use 1-byte (short) enums and their
  structs depend on it. The PS2's enums are 4 bytes. Host code is built
  with `-fshort-enums` and the game's code with `-fno-short-enums`, and no
  struct containing an enum is shared between them.
- **`va_list`:** it's a struct on ARM's ABI, not a pointer as on MIPS. The
  port's variadic-function shims (`port/include/ps2_compat.h`,
  `port/tools/host_sigs.py`) pass it through a transparent union.
- **Linker glue** (`port/tools/gen_link_glue.py`, `link_catalog.py`): the
  game's data is laid out contiguously in its original PS2 address order
  (`--sort-section=name`, `--no-gc-sections`), and names the game shares
  with devkitARM's libc are routed to the right side.

### Memory

The game expects the PS2's 32 MB main RAM and 16 KB scratchpad at fixed
addresses. The port keeps an image of PS2 memory (`port/host/ee_image.c`)
and relocates the scratchpad pointers into it at boot. The app uses a 1 MB
main stack and a 40 MB linear (GPU-visible) heap.

### CPU speed (New 3DS)

- `osSetSpeedupEnable(true)` switches on 804 MHz and the L2 cache.
- **PS2 float rounding in hardware:** the PS2's FPU rounds toward zero,
  flushes denormals and has no NaN/Inf. Fights only stay in sync with the
  real game if the port matches that. Instead of the slow software
  rewrite the web build uses, the 3DS sets the VFP control register to
  round-toward-zero + flush-to-zero + default-NaN (`FPSCR = 0x03C00000`),
  so every float operation rounds the PS2 way for free.
- **Frame pacing:** a 60 Hz accumulator on the system tick runs up to 3
  logic steps per displayed frame and skips the drawing for the extra ones.

### Graphics (`port/render/3ds/`)

The PS2 game draws through Capcom's "fl" graphics library. The upstream
port already replaced that library with a GL-style renderer. The 3DS
renderer (`r_c3d.c`) implements the same interface on **citro3d**:

- The game renders to a **640×480 canvas** inside a 1024×512 GPU render
  target, which is then scaled and rotated onto the 400×240 top screen
  (4:3 or stretched).
- The 3DS GPU (PICA200) has **no paletted textures**, and the game's
  sprites are 4-bit and 8-bit paletted. They're expanded to RGBA on the CPU
  through a cache keyed by texture + palette. Character sprites
  (`r_char_c3d.c`) go into a 64×64-cell tile cache that only re-uploads
  pages that changed.
- PICA textures are **Morton-tiled and stored bottom row first**. Getting
  that wrong was the "garbled graphics" stage of the port.
- The GPU runs a frame behind the CPU, so buffers the game frees or
  overwrites are kept (copy-on-write, deferred frees) until the GPU is done
  with them.
- 3D models go through `gl_shim.c`, a small GL subset, and are transformed
  on the CPU. The PICA vertex shaders for the GPU path (`mesh.v.pica`)
  are included but not used yet (see Known issues).

### Sound (`port/audio/audio.c`, 3DS section)

The game's mixer (ADX streams and sound banks) runs as on the web build,
outputting 48 kHz 16-bit stereo into **NDSP** with six 512-sample buffers.
The mixer runs on its own thread on **CPU core 2** (New 3DS only). If that
isn't available it falls back to core 1 with a 30% time limit, then to the
main core.

### Disc access (`port/platform/fs.c`, `port/platform/3ds/sys_3ds.c`)

Reads `.iso` (2048-byte sectors) and raw `.bin` images (2352-byte Mode 1/2,
2336-byte) by stripping the sector headers on the fly. Reads are batched 32
sectors at a time through a 64 KB buffer, because small SD reads on the 3DS
are slow. `.cue` sheets are parsed for the track's file name.

### Bottom screen (`port/platform/3ds/movelist_3ds.c`)

citro2d draws the move list, only when something changed. Player 1's point
character comes from the game's own player records in PS2 memory, which
hold the character ID. `port/tools/make_movelist.py` turns
`movelist_source.txt` into `movelist.txt`.

### Fixes in the shared port code

- `port/tools/host_overrides.py`: a tail call through a function table in
  `src/wave13b.c` is forwarded with its arguments, which fixed a crash
  mid-fight.
- `port/platform/fs.c`: BIN/CUE support, now shared by every target.

---

## Credits and licenses

- **The decompilation and the original port layer:**
  [g-guthrie/mvc2-ps2-decomp](https://github.com/g-guthrie/mvc2-ps2-decomp)
  and its contributors. This repository is a fork of it. The 3DS work
  lives in `port/cmake/3ds-clang.cmake`, `port/render/3ds/`,
  `port/platform/3ds/` and the 3DS sections of the shared port files.
- Third-party code in the decompilation keeps its own notices, see
  [LICENSES/](../../LICENSES/).
- **The move list text** comes from the
  [Marvel vs. Capcom Wiki](https://marvelvscapcom.fandom.com/wiki/Marvel_vs._Capcom_2:_New_Age_of_Heroes_move_list)
  (Fandom) and is used under **CC BY-SA 3.0**. Edits made for the 3DS
  screen are in `port/tools/make_movelist.py`. `movelist.txt` stays under
  CC BY-SA 3.0.
- devkitPro, libctru, citro3d and citro2d are by the devkitPro team and
  contributors.
- *Marvel vs. Capcom 2* is © Capcom, and the Marvel characters are © Marvel.
  This project isn't affiliated with or endorsed by Capcom, Marvel or
  Nintendo. It contains no game assets and needs your own legally obtained
  copy of the game.
