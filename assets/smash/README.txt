Smash 4 PC
==========

Super Smash Bros. for Wii U, running natively on Windows: the game's own code, translated ahead of time
into a PC program, with a Vulkan renderer. You bring the game.

This is a fan project. It is not affiliated with or endorsed by Nintendo, Bandai Namco or Sora Ltd.
No game files come with it: you need your own dump of the game from your own Wii U.


WHAT YOU NEED
-------------
- Windows 10 or 11 (64-bit) and a GPU with Vulkan 1.3 (NVIDIA GTX 900 series or newer, AMD RX 400 or newer, Intel Arc or recent integrated).
- Any 64-bit processor. smash.exe is built for AVX2 processors (Intel Haswell / AMD Ryzen and newer); on older
  ones (no AVX2) the launcher runs smash-compat.exe instead, which is slower and needs the update.
- Your dump of Super Smash Bros. for Wii U (USA), in the folder layout Cemu uses (code\, content\, meta\):
    - the game,        0005000010144F00   (needed)
    - the update v304, 0005000E10144F00   (recommended: all fighters, stages and fixes)
    - the DLC,         0005000C10144F00   (optional)
  Dump them from your Wii U with Dumpling.


GETTING STARTED
---------------
1. Unzip this folder anywhere you like (not inside Program Files).
2. Run "Smash 4 PC.exe" (the launcher).
3. Choose PLAY. The launcher asks for the game folder, then the update and the DLC (you can skip those two).
   It remembers them. You can also choose each folder on the main menu.
4. The first time you play, shaders compile as they are first seen (a few short hitches); after that they load
   at start-up from cache\.


CONTROLS
--------
- Gamepads: any XInput, PlayStation, Switch Pro or SDL-supported pad. Buttons go by position: the right face
  button is A, the bottom one B.
- The first pad (with the keyboard) is the Wii U GamePad. Every further pad is a Wii U Pro Controller, so up
  to 8 players can play locally (8-Player Smash): plug them in and press a button on the character select.
- Keyboard (the GamePad): W/A/S/D the D-pad, the arrow keys the stick, I/K/J/L the X/B/Y/A buttons, H and ;
  L and R, U and O ZL and ZR, Enter Plus, Backspace Minus.
  To change keys, write %USERPROFILE%\.config\ttt2\keyboard.ini (one "input = key" line each).
- Esc (or a pad's Home / Guide button): the in-game menu: frame counter, fullscreen, frame rate, quit.
- F11: fullscreen on or off.


SETTINGS (launcher, SETTINGS)
-----------------------------
- Resolution: Automatic follows your display; up to 4K and 5K.
- Display: window or fullscreen.
- Frame rate: 60, Automatic (120 on high refresh displays), 120 or Uncapped. Above 60 the game's frames are
  interpolated; the game itself always runs at 60.


MODS (launcher, MODS)
---------------------
- INSTALL MOD...: pick a mod's .zip, .7z or .rar (or drag the archive, or the mod's folder, onto the
  launcher's window). The launcher unpacks it into mods\ and switches it on.
- Each mod can be switched on or off; the first in the list wins where two change the same file.
- A mod is a folder holding the game's content\ layout (Sm4shExplorer packs, SDCafiine mods and Cemu graphic
  packs all work). Mods are made for the update (v304): they load only with it.
- OPEN MODS FOLDER shows mods\, if you'd rather manage the folders yourself.


NETPLAY (launcher, NETPLAY)
---------------------------
Play with friends on their PCs, up to 8 players, in any mode, stage, item setting or mod. The host's game is the
only one running: it is streamed to each friend (like Steam Remote Play Together), and their controller plays as
one of the host's Wii U Pro Controllers. Nothing can fall out of sync, and friends need no game files.
- Host: switch HOST MY GAMES on and choose PLAY. While you play, your room is listed for everyone.
- Join: the NETPLAY page lists the open rooms (players in each, up to 8). Pick one; the host's game opens in a
  window (F11: fullscreen) and your pads and keyboard play.
No router setup is needed: the two PCs connect directly. A few connections (some mobile and carrier networks)
don't allow that; join over a LAN or a VPN such as Tailscale instead (smash.exe --join ADDRESS).
Guests see the game in 1080p (encoded on the host's graphics card), with the delay to the host added; a close
connection plays best. The host sends about 12 Mbit/s to each guest, so check your upload speed for big rooms.


ONLINE (launcher, ONLINE)
-------------------------
Smash's own online modes work on Pretendo Network, the community replacement for Nintendo Network:
1. On your Wii U, run Dumpling and choose "Dump Online Files".
2. In the launcher's ONLINE page, choose CONSOLE FILES and pick the folder Dumpling made (it finds otp.bin,
   seeprom.bin and your account in it).
3. Type your Pretendo Network ID and password and choose SIGN IN.
Your password is never saved: only the console's hash of it, as on a Wii U. Your console files and account
stay in online\ beside the program; never share that folder. You need a Pretendo Network account
(pretendo.network) and a console that is in good standing there.


WHERE THINGS ARE
----------------
- save\       your save (Smash's data, custom stages, photos)
- online\     your console files and account (private)
- mods\       installed mods
- cache\      compiled shaders (safe to delete; they compile again)
- smash.log   the game's messages from the last run: include it when you report a problem


TROUBLESHOOTING
---------------
- The game closes with an error: the launcher shows the error; smash.log has the details.
- Black screen or a crash at start: update your GPU driver (Vulkan 1.3 is needed).
- Mods don't show: they need the update folder chosen, and content\ somewhere inside the mod's folder.
- Online sign-in fails: check the console files are your own Wii U's and the console isn't banned on Pretendo.
- A screen waits and nothing you press works: check smash.log's "pad connected" lines. The first pad that
  presses a button becomes the GamePad, so press a button on the pad you play with.
- Netplay: a room doesn't show or can't be joined: Windows' firewall must allow smash.exe (it asks the first
  time you host); on very strict networks use a LAN or Tailscale.


LICENSES
--------
Smash 4 PC's own code is under the MIT license (LICENSE.txt). It uses SDL3, the Vulkan headers, shaderc,
glslang, SPIRV-Tools, zlib, zstd, ZArchive, AMD's addrlib, stb and parts of Cemu's Nintendo Network code
(MPL 2.0); their licenses are in licenses\, with the MPL-covered Cemu source in licenses\cemu_nex-source\.
