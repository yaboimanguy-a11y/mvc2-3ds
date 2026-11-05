Marvel vs. Capcom 2 (PS2 decomp) - Nintendo 3DS
================================================

NEW Nintendo 3DS / New 3DS XL / New 2DS XL ONLY (the original 3DS and
2DS are too slow).  Full guide: port/docs/3DS.md in the repository.

1. Put your own disc image of the game in this folder. It must be a
   dump of the NTSC-U PS2 disc (SLUS-20486), in any of these forms
   (any file names):

       a .iso
       a .bin and its .cue
       a .bin on its own (the .cue is optional)

2. Copy this whole "mvc2" folder to your SD card's "3ds" folder, so
   the card has, for example:

       /3ds/mvc2/mvc2.3dsx
       /3ds/mvc2/Marvel vs. Capcom 2 (USA).bin
       /3ds/mvc2/Marvel vs. Capcom 2 (USA).cue

3. Start the game from the Homebrew Launcher, OR install mvc2.cia (from the
   release page) with FBI and start it from the HOME Menu.  One or the
   other is enough; both read their files from this folder:

   "Marvel vs. Capcom 2" (mvc2.3dsx)
       The game on the top screen, with sound.  Sound needs the 3DS DSP
       firmware, sdmc:/3ds/dspfirm.cdc: if you have never made it, run the
       "DSP1" homebrew once (it writes that file).
       D-pad / Circle Pad = directions
       Y = Square  X = Triangle  B = Cross  A = Circle
       L = L1  R = R1  ZL = L2  ZR = R2  START  SELECT
       Bottom screen: the move list of your point character (it follows
       tags), drawn as what to press: arrows for the stick and the 3DS
       buttons (Y light punch, X heavy punch, B light kick, A heavy kick,
       L / R assists); gold moves are hyper combos.  The list comes from
       movelist.txt in this folder: edit it freely.
         touch the name row (left / right): through your team
         touch the list: next page
         touch the cog (bottom left): settings - frame skip on/off,
           4:3 or widescreen, logging (mvc2_log.txt, off by
           default), characters (all unlocked, or bought in the shop as on
           the PS2; each has its own save - mvc2_memcard_unlocked and
           mvc2_memcard - and a change applies at the next start), and
           "Check for updates", which downloads
           the newest release from GitHub over Wi-Fi and installs it in
           this folder, and the HOME Menu app too when mvc2.cia is
           installed (your disc image and saves stay; restart after)
       Frame skip (on by default) keeps the game at full speed when the
       3DS can't draw every frame.
       3D slider up: stereoscopic 3D (the stage sits behind the fighters).
       The picture is drawn once per eye then, so busy scenes can run a
       little slower with 3D on.
       L + R + START + SELECT: quit.

With logging on (settings), the game writes mvc2_log.txt in this folder,
replaced on every run: what happened, and every second the frame rate and
where each frame's time went ("time:" and "steps:" lines).  If the game
crashes, Luma3DS saves a dump in sdmc:/luma/dumps/arm11/; send it with the
log.

No game data is included. You supply the disc image.
