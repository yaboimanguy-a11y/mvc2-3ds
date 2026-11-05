Marvel vs. Capcom 2 (PS2 decomp) - Nintendo 3DS
================================================

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

3. Start a program from the Homebrew Launcher:

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
         touch the bottom row: left = frame skip on/off, right = 4:3 / stretch
       Frame skip (on by default) keeps the game at full speed when the
       3DS can't draw every frame.
       L + R + START + SELECT: quit.

   "MvC2 renderer test" (mvc2_rtest.3dsx)
       Needs no disc. Draws a test picture, checks it, and writes the
       result to mvc2_rtest_log.txt in this folder.

   "MvC2 boot test" (mvc2_boottest.3dsx)
       Runs the game's logic with no picture and measures its speed.
       START stops it and prints a summary.

The programs write mvc2_log.txt (game, boot test) and mvc2_rtest_log.txt
(renderer test) in this folder, replaced on every run.

No game data is included. You supply the disc image.
