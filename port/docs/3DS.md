# Marvel vs. Capcom 2 on the Nintendo 3DS (v1.0.1)

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
- [Known issues in v1.0.1](#known-issues-in-v101)
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

2. **Download the release.** Get `mvc2-3ds-v1.0.1.zip` (and `mvc2.cia` if you want a HOME Menu icon) from the
   [Releases](../../../releases) page and unzip it. Inside is a folder `3ds/mvc2/`:

   ```
   3ds/mvc2/mvc2.3dsx            the game
   3ds/mvc2/movelist.txt         the bottom screen's move list
   3ds/mvc2/cacert.pem           certificates for "Check for updates"
   3ds/mvc2/README.txt           short version of this guide
   ```
   The release also has `mvc2.cia`, the same game as a HOME Menu app
   (install it with FBI). **You need one or the other, not both:** start
   `mvc2.3dsx` from the Homebrew Launcher, or install `mvc2.cia` and start
   it from the HOME Menu. Either way the folder above (without
   `mvc2.3dsx` if you only use the CIA) and your disc image are needed:
   both read the disc image, `movelist.txt`, `cacert.pem`, settings and
   saves from `/3ds/mvc2/`, so having both is fine too. The test programs
   and the profiler below are built with the project
   (`build/port-3ds/sdcard/`), not released.

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
| **3D slider** | | Stereoscopic 3D (see below) |

You can remap buttons in the game's own options menu. The move list always
shows the default layout above.

**Stereoscopic 3D.** Slide the 3D slider up and the stage's 3D geometry
gets depth: it sits behind the screen, the fighters and their effects stay
on the screen plane, and the slider sets how deep it goes. With the slider
down, the game draws one picture as usual. With 3D on, the GPU draws every
frame twice (once per eye), so very busy scenes can run a little slower.

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
| The cog, bottom left | Settings: frame skip, 4:3 or widescreen, logging (off by default), characters (all unlocked or bought in the shop, each with its own save), **Check for updates** |

Settings are kept in `settings.txt` in the app's folder. **Characters: Buy
in the shop** gives the PS2's own progression, but a save that was made with
everything unlocked stays unlocked: delete `/3ds/mvc2/mvc2_memcard/` to
start progression from scratch. **Logging** is off by default; the game's
log (`mvc2_log.txt`, with the timing lines below) is only written with it on.

**Check for updates** asks GitHub for the newest release (Wi-Fi needed),
and when there is a newer one, a second tap downloads its zip and writes
the files of its `3ds/mvc2/` folder over the ones on the SD card. When the
game runs as the CIA, or the CIA is installed, it installs the release's
`mvc2.cia` too, so the HOME Menu app stays on the same version. The disc
image, saves and logs are left alone; quit and start the game again to use
the new version. The connection is checked against `cacert.pem` in the
app's folder.

The list comes from `movelist.txt` in the app folder, a plain text file
you can edit. The format is described at the top of the file.

## Online VS

**VS** on the main menu is online play against other New 3DS consoles,
one player per console, over Wi-Fi. Choosing VS opens the **Online VS**
screen on the bottom screen instead of the local two-player mode:

| Button | Does |
| --- | --- |
| **Find rooms** | The public rooms: name, mode, players. Tap one to join. |
| **Create room** | Host a room: **Mode** 1v1 or King of the Hill, **Public** (listed in Find rooms) or **Private** (joined with its code). The room screen shows its 6-digit **code**. |
| **Join code** | Type a room's 6-digit code (or a host's IP address) on the keypad. |
| **Leaderboard** | The ratings of everyone who played online, and your rank. |
| **Sync test** | One console: checks that it keeps a match in sync (see below). |

**Modes.** Up to 8 people per room; whoever isn't playing watches the
match live on their own 3DS.
- **1v1:** the host and the first player to join play VS, with rematches
  until one of them leaves; anyone else in the room watches.
- **King of the Hill:** two play; the winner stays on, the loser goes to
  the back of the line, the next in line challenges. Wins are counted on
  the room screen (`*` host, `+` next in line, `>` you).

The host taps **Start** when at least two are in the room. Everyone in the
room then restarts the game into VS's character select (a few seconds);
the next King of the Hill round starts by itself 6 seconds after a fight
ends. **Leave** takes you out of the room (the host leaving closes it).

**Leaderboard.** After every fight, both players' consoles report the
result to the lobby server, and it counts only when the two reports agree.
Each counted fight moves both players' ratings (Elo, starting at 1000) and
their wins, losses, draws and best win streak. The Leaderboard screen shows
the top players and your rank; the same list is a web page at
<https://mvc2-lobby.yaboimanguy.workers.dev>. Your player id and its secret
key are made once and kept in `/3ds/mvc2/online_id.txt`: keep that file
(copy it to a new SD card) to keep your rating. The same two players count
at most 20 times a day.

**How it connects.** A room is hosted by the host's 3DS: the matches go
directly between the consoles. A small lobby server (a free Cloudflare
Worker, `port/server/lobby/`) only lists the public rooms, turns codes into
addresses, and tells the host who is joining so both consoles send to each
other at once, which gets through most home routers without port
forwarding. When two players can't reach each other directly, the room's
host passes their inputs on. To learn its internet address, each 3DS asks a
public STUN server (`stun.l.google.com`) once; nothing else is sent there.
Without a lobby address (see `port/server/lobby/README.md`) rooms can still
be joined by the host's IP address (the room screen shows the same-Wi-Fi
one; over the internet the host's router must forward UDP port 27886).

Everyone needs **the same app build** (the same release file) and **the
same game disc** (the US disc, SLUS-20486); a room refuses others and says
which one differs.

**How a match runs.** Each console restarts the game to its power-on state,
with an empty online memory card (`mvc2_memcard_online/`, your own save is
not touched), every character unlocked and the same clock. All of them boot
through the title and the main menu into VS on their own, in fast forward
with the sound off, and from the character select they run the same frames
with the same two pads. Every press is delayed by a fixed **3 frames
(50 ms)**, which covers a round trip of up to about 60 ms with no slowdown;
over a slower connection the game slows down (about 51 fps at a 100 ms
ping, 34 fps at 160 ms) instead of skipping ahead. The ping shows on the
room screen and in the footer. The result of each fight is read from the
game (the side with characters left standing, or with more health at time
over). When you leave the room after playing, the game restarts once more
for normal play with your own save.

**Sync test.** Online play only works if every console computes exactly
the same game. The sync test checks that on one console: it restarts the
game, boots into VS and plays 50 seconds of random presses for both
players, then does all of it again and compares a fingerprint of the
game's memory on every frame. It takes about two minutes and then says
*passed* or *failed*. Please run it once and report the result.

**If a match goes out of sync**, the room screen says *Out of sync at frame
N*, the consoles involved write `mvc2_desync_m<member>_f<frame>.bin` (the
game's memory at the same frame) next to the app, and the log has the
details. Please send those files and `mvc2_log.txt` from both consoles (turn
logging on in the settings first). A failed sync test writes
`mvc2_synctest_run2.bin` and `mvc2_synctest_run3.bin` the same way.

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
| Graphics look wrong | Send a photo and the log. Developers: the renderer self-test (`mvc2_rtest.3dsx`, built with the project) checks the GPU output without a disc. |
| Console crashes (Luma exception screen) | Luma saves a dump to `sdmc:/luma/dumps/arm11/`. Open an issue with that `.dmp` file and `mvc2_log.txt`. |
| Slow in some scene | The log's `time:` and `steps:` lines (every second) say where the frame time went. For a closer look, run **MvC2 profiler** (`mvc2_prof.3dsx`, below). |

### The timing lines in the log

Every second the game logs, besides the frame rate, where each frame's time
went (milliseconds per frame). The layout (these numbers are only an
illustration, not a measurement):

```
f  4380 st 020102 59.8fps (game 59.8/s, 0 skipped) logic 9.1/11.0ms gpu 8.2ms ...
  time: vblank 4.1 logic 9.1 (mesh 1.2 char 0.8 tex 0.0 sndlock 0.05) gpuwait 0.3 submit 0.9 ui 0.0 end 0.2, longest frame 18.1 | ...
  steps: game update 5.3 render pass 1 2.1 render pass 2 0.9 sound tick 0.4 ...
```

- `logic`: the game's code, including recording its draws (`mesh`, `char`
  and `tex` are parts of it); `steps:` splits it by the game's own steps.
- `gpuwait`: waiting for the GPU to finish the previous frame (high means
  the GPU is the bottleneck).
- `vblank`: time to spare, waiting for the next screen refresh.
- `submit`: sending the recorded frame to the GPU (per eye with 3D on).

### The profiler build (`mvc2_prof.3dsx`)

The same game with every function of the game, host and renderer code
instrumented: a second CPU core samples which function is running about
2000 times a second, and every 30 seconds the log lists the functions with
the most time spent in them ("self") and under them ("inclusive"), by
address. It runs somewhat slower than the normal build. To name the
addresses, use the `mvc2_prof.elf` of the same build:

```bash
python3 port/tools/prof_symbolize.py build/port-3ds/mvc2_prof.elf mvc2_log.txt
```

---

## Known issues in v1.0.1

- **Speed.** Fights run at full speed, 60 fps. The very busiest moments
  (several hyper combos and assists at once) can still drop a few frames;
  frame skip (on by default) keeps the game's speed then. On the 3DS the
  sound effects are resampled with two-point interpolation (the PS2 used
  four) to leave the CPU time for the game.
- **Online VS** needs the same app version and game disc on every console;
  a ping over about 60 ms makes the game slow down rather than skip.
- **3D models are transformed on the CPU** (on the New 3DS's second core).
  The GPU's vertex-shader path for the stages hung the console in testing.
  Textured 3D models are drawn without fog.
- **The picture is drawn at 480x360** (three quarters of the PS2's
  640x480) and stage textures use 16 bits where that loses nothing visible,
  to keep the GPU within the frame time. That is still finer than the top
  screen's picture.
- **Stereoscopic 3D is a first version.** The depth comes from the stage's
  3D geometry; 2D stage layers drawn before the fighters follow it, the
  rest stays on the screen plane.
- **Characters: Buy in the shop** has its own save (`mvc2_memcard/`); a
  change of that setting applies at the next start.
- **Not yet done:** Old 3DS support, button remapping for the move list.

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

The versions used for v0.2.1: devkitARM r68, libctru 2.7.1, citro3d 1.7.1,
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
- **Frame pacing:** one logic step per refresh of the top screen
  (59.83 Hz), in phase with it: when ahead, the loop waits for the refresh;
  when a frame ran long, the next one runs the steps that are due (up to 3)
  and draws only the last.
- **CPU and GPU in parallel:** the renderer records each frame's draws
  while the game's logic runs and hands them to the GPU afterwards
  (below), so the GPU draws frame N while the CPU runs the logic of frame
  N+1. Before, the two took turns.
- **No whole-heap cache flush:** citro3d's `C3D_FrameEnd(0)` writes the
  whole 40 MB GPU heap out of the CPU cache every frame. The app flushes
  what it wrote (vertex ring, textures, character tile cells) and ends
  frames with `GX_CMDLIST_FLUSH`; only a frame that redrew the bottom
  screen (citro2d) does the full flush.
- **Profiling:** zone timers (`port/render/3ds/r3ds_prof.h`) and the game's
  own steps are timed every frame and logged every second; the profiler
  build samples functions (`port/platform/3ds/prof_fn_3ds.c`).

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
- **Draw records:** every draw becomes a record (render state + a range of
  the frame's vertex ring). The app keeps them while the logic runs and
  replays them after `C3D_FrameBegin`; the replay sets the vertex buffer
  once and emits only the state that changed between records (texture,
  combiner, blend, depth, scissor, uniforms), where it used to rebuild the
  whole pipeline for every draw.
- The GPU may still be drawing the previous frame while the logic writes
  the next, so textures, palette expansions, character tile cells and
  freed buffers that either frame uses are kept (copy-on-write, deferred
  frees) for two frames.
- **Stereoscopic 3D:** the records are replayed once per eye into the
  same canvas, each eye presented to its own top-screen target (citro3d
  shows a frame in 3D when the right eye was drawn). The 2D vertex shader
  adds `clamp(a + b·z, lo, hi)` to each vertex's x: per eye `a`, `b` follow
  from the depth range of the stage's meshes (smoothed over frames), so its
  far end goes about 10 screen pixels deep at full slider and the nearest
  part comes out by up to 3. Fighters (character tile pages), what is drawn
  after them, clears and the present stay at zero.
- Palette writes that don't change any colour keep the palette's version,
  so they no longer flush the batch or rebuild expansions and tiles.
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

### Online VS (`port/platform/3ds/online_3ds.c`, `port/platform/net/room.c`, `port/host/game_restart.c`)

- **Restart:** `mvc2_restart()` puts the game's memory back to power-on:
  the EE data image is copied at the first boot and copied back, the EE
  BSS, scratchpad and heap are zeroed (the heap keeps its address, aligned
  to 64 KB), and each host module that answers the game's library calls
  resets its own state (card files, ADXF handles, semaphores, the sound
  client's queue, pads, frame count). The frontend drops its caches
  (`fe_restart`: texture and palette records, character banks, clay meshes)
  and the sound engine its banks and voices (`audio_restart`). Then
  `mvc2_boot(1)` runs again.
- **Determinism:** the game step is a function of memory and pads; the RTC
  is fixed (`host_clock_fixed`), and the one sound status the game reads
  for a decision that depends on the mixer's timing (whether a voice still
  plays, `func_001E59E0`) answers "playing" (`audio_set_deterministic`).
- **Rooms and lockstep** (`room.c`): the host's console keeps the room
  (members, scores, King of the Hill's queue) and sends it to everyone; a
  match is a session every member restarts for. UDP, one datagram per frame
  on each link with every input the far end has not acknowledged (a lost
  datagram is repaired by the next), so the host relays the players' inputs
  to the watchers, and between two players who can't reach each other. A
  fixed delay of 3 frames; GGPO-style time sync between the two players.
  Tested on a desktop by `port/tools/room_test.c` (up to six simulated
  consoles: latency, jitter, loss, blocked paths, people leaving).
- **Lobby directory** (`port/server/lobby/worker.js`, `lobby_3ds.c`):
  HTTPS on a background thread; rooms check in every 5 seconds and are
  forgotten 45 seconds after the last. `node port/server/lobby/test.mjs`
  tests it without a Cloudflare account.
- **Fingerprints:** `mvc2_state_hash_frame` hashes 1/32 of the data image,
  BSS and scratchpad plus 64 KB of the heap each frame (every byte every few
  seconds), compared between the consoles.

### Bottom screen (`port/platform/3ds/movelist_3ds.c`)

citro2d draws the move list, only when something changed. Player 1's point
character comes from the game's own player records in PS2 memory, which
hold the character ID. `port/tools/make_movelist.py` turns
`movelist_source.txt` into `movelist.txt`.

### Fixes in the shared port code

- `port/tools/host_overrides.py`: calls whose callee reads an argument the
  PS2 left in a register (`$a1`) but the decompiled call does not pass get
  it passed explicitly: a tail call through a function table in
  `src/wave13b.c`, the grab / throw callback in `src/q6_x.c` (v0.2), and the
  effect node dispatch in `src/n6.c` (v0.2.1). Each fixed a crash mid-fight.
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
