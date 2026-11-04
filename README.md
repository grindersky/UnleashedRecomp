<p align="center">
    <img src="https://raw.githubusercontent.com/hedge-dev/UnleashedRecompResources/refs/heads/main/images/logo/Logo.png" width="512"/>
</p>

---

# Unleashed Recompiled: TAS mode

This branch adds a TAS mode to Unleashed Recompiled for [libTAS](https://github.com/clementgallet/libTAS) on Linux, and on Windows through WSL: deterministic movie playback, savestates, correct audio at any libTAS frame rate, libTAS's on-screen display and Stop button, and an optional HUD for TASing. Without the TAS mode turned on, the game runs like the normal port.

> [!TIP]
> Under WSL, libTAS runs the game on the CPU, which is slow (about 12 fps when fast-forwarding). Once the game is set up, follow the [dozen guide](DOZEN.md) to play back and encode movies on your graphics card instead, several times faster.

## Setting Up the TAS Version

### 1. Set up WSL and libTAS

On Windows, follow libTAS's [WSL guide](https://clementgallet.github.io/libTAS/guides/wsl/): it installs WSL 2 with Ubuntu, then libTAS. Everything below runs in that Ubuntu terminal. On Linux, just install libTAS.

The TAS mode was tested with libTAS 1.4.8 on Ubuntu 26.04 in WSL 2.

### 2. Install the build dependencies

```bash
sudo apt install git autoconf automake libtool pkg-config curl cmake ninja-build clang clang-tools libgtk-3-dev
```

### 3. Get the source

Clone this branch:

```bash
git clone --recurse-submodules -b tas-mode https://github.com/grindersky/UnleashedRecomp.git
cd UnleashedRecomp
```

### 4. Add the game files needed to build

Copy `default.xex`, `default.xexp` and `shader.ar` from your copy of the game into `UnleashedRecompLib/private/`.

If you already have Unleashed Recompiled installed, for example on Windows, take them from its folder: `game/default.xex`, `game/shader.ar` and `update/default.xexp`. From WSL, Windows drives are under `/mnt`, for example `/mnt/c/Games/UnleashedRecomp/game/default.xex`. Otherwise, see [Add the Required Game Files](docs/BUILDING.md#2-add-the-required-game-files).

### 5. Build

```bash
cmake . --preset linux-release
cmake --build ./out/build/linux-release --target UnleashedRecomp
```

The first build takes a while, as it recompiles the whole game. The game ends up in `out/build/linux-release/UnleashedRecomp/`.

### 6. Install the game data

Run the game once on its own, without libTAS:

```bash
cd out/build/linux-release/UnleashedRecomp
./UnleashedRecomp
```

The installer asks for your game, its title update and, optionally, the DLC (see [How to Install](#how-to-install)). Once you reach the title screen, quit the game.

For the game, it's recommended to just select your Xbox 360 `.iso` with **Add Files**. Don't forget the title update ("Patch Update") file as well, for example `TU_19KA20I_0000004000000.00000000000G1`: the game can't be installed without it.

### 7. Set up libTAS

Start libTAS with the TAS mode turned on:

```bash
UNLEASHED_TAS_MODE=1 libTAS
```

Choose `out/build/linux-release/UnleashedRecomp/UnleashedRecomp` as the game executable. Then, with the game closed, set these in libTAS's **Settings**:

- **Runtime → Prevent writing to disk**: on. Playing a movie then can't change your save, so every movie starts from the same save.
- **Video → Force software rendering**: on. The game then draws with lavapipe, Mesa's renderer that runs on the CPU, which the TAS mode was made and tested with, savestates included.
- **Input → Mouse support**: off, as the game doesn't use the mouse. **Joystick support**: 1 controller, for analog stick inputs.

> [!TIP]
> Software rendering is slow. To play back and encode movies on your graphics card, several times faster, set up dozen with the [dozen guide](DOZEN.md). Keep software rendering on while making TASes: loading a libTAS savestate on dozen crashes the game.

> [!NOTE]
> Savestates on the graphics card are in development in [jakobhellermann's libTAS](https://github.com/jakobhellermann/libTAS): its "Forwarded GPU" rendering keeps the GPU's state in a separate process that it saves along with the game. The TAS mode works with it (under WSL with `LIBTAS_GPU_MODE=COPYMODE`, as WSL can't share GPU memory with the game), but it isn't part of a libTAS release yet and setting it up isn't covered here.

Start the game from libTAS and TAS as usual.

### Updating the TAS version

When the TAS version gets an update, update the source, then build the game again:

```bash
cd UnleashedRecomp
git pull
git submodule sync --recursive
git submodule update --init --recursive
```

Then build the game again, as in step 5:

```bash
cmake . --preset linux-release
cmake --build ./out/build/linux-release --target UnleashedRecomp
```

There's no need to install the game data again: it stays next to the game in `out/build/linux-release/UnleashedRecomp/`, and your saves, mods, codes and libTAS settings stay too. Only a fresh clone needs step 6 again, or those folders (`game`, `update`, `dlc`, `patched`) copied over from the old one.

### Options

Set these in front of `libTAS`, like `UNLEASHED_TAS_MODE=1`:

- `UNLEASHED_TAS_MODE=1`: turns the TAS mode on. Without it, the game runs like the normal port.
- `UNLEASHED_TAS_HUD=1`: a HUD, shown in levels. It's drawn into the frames, so it also shows up in encodes.
  - As Sonic: his speed, his movement mode (free 3D, forward, 2D, running on water or along a wall, on ice, a boss fight's special mode, or diving), his state (on the ground or in the air, sliding, stomping, and whether a jump, stomp, quick step, homing attack (once the game has a target) or boost pressed on the current frame would work, with the boost gauge and countdowns until a stomp is allowed again after a jump or an air boost, and until the next quick step), whether he is out of control (after a dash panel or a run quick step: "OUT OF CONTROL Nf", where presses do nothing for the next N frames; "until landing" when it only ends on the ground, and no number when it has no timer), the stick with the M-Speed zone (in 3D, on the side of the stick Sonic faces: green where it works, red where it doesn't), the D-Speed zone (in 2D, a strip where the stick's part along the path counts on the next frame, which moves as the game turns its stick direction, coloured the same way, with "air brake" when Sonic brakes in the air instead) and the break-jump zone, and the buttons.
  - As the Werehog: his speed, his state (on the ground or in the air, dashing, attacking, which attack and in how many frames it ends on its own, with and without the stick), whether a guard (LB) pressed on the current frame would work, his Unleash gauge, the stage time, the stick and the buttons.
  - Over each hint ring on screen: whether touching it now would count. A hint ring ignores touches for 2 seconds after the game creates it (as Sonic gets near) and after its hint closes, so at speed Sonic can go through one without anything happening. The label shows "HINT ready in Nf" (a touch counts from N frames after the current one), "HINT ready" or "HINT touched". Only Sonic's body counts, and the box that has to be touched is much smaller than the ring of orbs: about 1.1 m wide, 1.5 m tall and 0.5 m deep, around the question mark.
  - A radar above the panel, as day Sonic: Sonic seen from above, with the stage's walls and floor edges around his height, the rings, enemies and goal ring around him, and in the air an estimate of where he reaches the next ring's height (without stick input or walls). Press Home to change how far it reaches and M to hide or show it. Under libTAS, both keys are recorded into the movie like the game's keys.
  - A TAS camera: press `\` to switch between the game's camera, a camera behind Sonic along his facing and one looking down on him from above. It only changes what's drawn, not the game, so a movie plays back the same with any of them. Things the game's own camera doesn't see can still be left out. Under libTAS, `\` is recorded into the movie like the game's keys.

### Mods and codes

The game loads mods and Hedge Mod Manager codes through two files, which Hedge Mod Manager usually writes. In WSL, you don't need Hedge Mod Manager: write them yourself. Edit them while the game is closed, as it only reads them when it starts.

> [!TIP]
> On Windows, [TAS Mods and Codes](tools/TasModsAndCodes/README.txt) does this for you: a window to tick and order the mods and codes, configure mods, copy Hedge Mod Manager's mod list, and change the game's graphics settings. Copy the `tools/TasModsAndCodes` folder to Windows (it doesn't start from inside WSL's own files), then double-click `TAS Mods and Codes.cmd`. It writes `ModsDB.ini`; `cpkredir.ini` below is still needed.

**`~/.config/UnleashedRecomp/cpkredir.ini`** turns mods and codes on, and says where to find the list of them. Write the full path, as `~` doesn't work there:

```ini
[CPKREDIR]
Enabled=1
ModsDbIni="/home/YOUR_NAME/.config/UnleashedRecomp/ModsDB.ini"
```

**`ModsDB.ini`**, the file `ModsDbIni` points to, lists the active mods and codes:

```ini
[Main]
ActiveModCount=2
ActiveMod0=CustomStage
ActiveMod1=OtherMod

[Mods]
CustomStage="/home/YOUR_NAME/UnleashedMods/CustomStage/mod.ini"
OtherMod="/mnt/c/UnleashedMods/OtherMod/mod.ini"

[Codes]
Code0="HomingAttackOnJump"
Code1="SkipIntroLogos"
CodeCount=2
```

- **Mods**: extract each mod into its own folder, with its `mod.ini` directly inside. Give it any name, point that name to its `mod.ini` under `[Mods]`, and list it under `[Main]` as `ActiveMod0`, `ActiveMod1` and so on, with `ActiveModCount` set to how many there are. Mods made for Hedge Mod Manager and for Unleashed Mod Manager both work. If two mods replace the same file, the one listed first wins.
- **Custom levels** are mods that replace one of the game's stages. Turn the mod on, then play the stage it replaces; its description says which one. To switch to another custom level, change which mods are listed under `[Main]`: mods only listed under `[Mods]` aren't loaded.
- **Codes**: list them under `[Codes]` as `Code0`, `Code1` and so on, with `CodeCount` set to one more than the highest number. To turn a code off without renumbering the others, put a `;` in front of its line. The names must be written exactly as below:

  `AllowCancellingUnleash`, `DisableAutoSaveWarning`, `DisableBoostFilter`, `DisableDLCIcon`, `DisableDPadMovement`, `DisableDWMRoundedCorners`, `DisableLowResolutionFontOnCustomUI`, `EnableEventCollisionDebugView`, `EnableGIMipLevelDebugView`, `EnableObjectCollisionDebugView`, `EnableStageCollisionDebugView`, `FixEggmanlandUsingEventGalleryTransition`, `FixUnleashOutOfControlDrain`, `HomingAttackOnJump`, `HUDToggleKey`, `RestoreReadyGoSkip`, `SaveScoreAtCheckpoints`, `SkipIntroLogos`, `UseAlternateTitle`, `UseArrowsForTimeOfDayTransition`, `UseOfficialTitleOnTitleBar`
- **Collision views**: `EnableStageCollisionDebugView` shows the stage's collision instead of its models, `EnableEventCollisionDebugView` the event triggers, and `EnableObjectCollisionDebugView` the objects' collision. Press F11 to switch between the views and the level as it normally looks. With the stage collision view, the grind rails' collision is drawn too (orange), and with any of the three:
  - the player's collision that's on: his body (cyan), what touches rings, objects and triggers (magenta, green while day Sonic attacks, like when he slides), and day Sonic's path collision (blue), which most likely touches paths like the grind rails,
  - an arrow at his feet, pointing where he faces,
  - where the views don't already show them, the enemies' collision (red) and the goal ring's (white).

  These are hidden behind walls; press F12 to switch to drawing them over everything, walls included, and back. Hiding them needs anti-aliasing off: with it on, they're always drawn over everything. Under libTAS, F11 and F12 are recorded into the movie like the game's keys. In TAS mode, the views start shown only with `UNLEASHED_TAS_HUD=1`, and the level starts as it normally looks without it: a movie without F11 presses then shows the views with the HUD and the normal level without it. Either way it plays back the same, as F11 only changes what's drawn. Turning the codes themselves off does change sync: keep them as they were when the movie was recorded.
- **Ready... GO! skip**: `RestoreReadyGoSkip` brings back a glitch the title update fixed. Pause during a stage's in-game cutscene and restart: Sonic can move during Ready... GO!

### Keeping movies in sync

libTAS doesn't save everything that matters in a movie. Play movies back with:

- the same mods, mod options and codes enabled as when the movie was recorded (see [Mods and codes](#mods-and-codes)),
- the same save.

Graphics settings (resolution, anti-aliasing, shadow resolution, motion blur...) and the renderer (lavapipe, dozen) don't affect sync: TAS with fast settings, encode with high ones. The game's FPS option doesn't matter either: in TAS mode, the game steps by libTAS's frame rate.

Movies recorded at frame rates other than 60 fps with a version from before 2026-09-28 will probably desync, and so may movies recorded before 2026-10-26 with the game's FPS option different from libTAS's frame rate.

### Faster playback and encoding

To play back and encode movies on your graphics card instead of the CPU under WSL, see the [dozen guide](DOZEN.md). It makes playback and encoding several times faster.

