TAS Mods and Codes
==================

A small Windows window for picking the mods and codes that the Linux (WSL) TAS build of
Unleashed Recompiled loads, instead of editing ModsDB.ini by hand.

What you need
- Windows 10 or 11 with WSL, and the Linux TAS build of Unleashed Recompiled in it.
- Your mods in a Windows folder, for example Hedge Mod Manager's mods folder.
Nothing else to install: it runs on the Windows PowerShell that comes with Windows.

Start it
1. Put this folder in a Windows folder: extract the zip anywhere, or copy the folder from the
   TAS version's source (tools/TasModsAndCodes) to Windows, for example from WSL:
     cp -r tools/TasModsAndCodes /mnt/c/Users/YOUR_NAME/
   It doesn't start from inside WSL's own files (\\wsl$\...).
2. Double-click "TAS Mods and Codes.cmd".
   If Windows says "Windows protected your PC", click "More info", then "Run anyway".
   (To avoid that: before extracting, right-click the zip > Properties > tick "Unblock" > OK.)
3. On the first start, if it can't find your mods folder, it opens Settings: pick the folder
   with your mods (one folder per mod, each with a mod.ini).
Tip: right-click the .cmd > Send to > Desktop (create shortcut) for a desktop shortcut.

Using it
- Tick the mods to load. Drag them to change the order: the higher one wins when two mods
  change the same file. Drag a mod into "Not loaded" to turn it off.
- Mods with a gear have options: right-click > Configure, or the Configure... button.
  Options are saved in the mod's own folder, so Hedge Mod Manager shows the same ones.
- "Copy Hedge Mod Manager's mods" takes Hedge Mod Manager's mod list and order (not its codes).
- Tick the codes on the right.
- Press Save. The game reads mods and codes only when it starts: restart it after saving.
- "Save current as..." keeps a named setup (for example per movie) that "Load" brings back.

Graphics
"Graphics..." changes the game's config.toml: resolution (presets from 480p to 8K, or your own
size), resolution scale, FPS, anti-aliasing, shadow resolution, GI texture filtering and motion
blur. Close the game first: it saves its settings when it closes, which would undo the change.
Graphics settings don't change sync: TAS with fast settings, encode with high ones.

Sync
libTAS movies don't store mods, mod options or codes. Play a movie back with exactly the setup
it was recorded with, or it can desync. The collision view codes count too: keep them as they
were when recording. To hide the views, start libTAS without the TAS HUD (in TAS mode they then
start hidden), or press F11 in the movie: neither affects sync.

Settings
- WSL distro: the Linux system the game runs in (default: your default WSL distro).
- Mods folder: where your mods are.
- Linux build folder (optional): only for a portable install (portable.txt next to the game)
  or a build folder with its own cpkredir.ini.
The tool writes the game's ModsDB.ini (normally ~/.config/UnleashedRecomp/ModsDB.ini in WSL)
and keeps the previous one as ModsDB.ini.bak.
