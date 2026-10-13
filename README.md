<p align="center">
    <img src="https://raw.githubusercontent.com/hedge-dev/UnleashedRecompResources/refs/heads/main/images/logo/Logo.png" width="512"/>
</p>

---

# Unleashed Recompiled: TAS mode

This branch adds a TAS mode to Unleashed Recompiled for [libTAS](https://github.com/clementgallet/libTAS) on Linux, and on Windows through WSL: deterministic movie playback, savestates, correct audio at any libTAS frame rate, libTAS's on-screen display and Stop button, and an optional HUD for TASing. Without the TAS mode turned on, the game runs like the normal port.

> [!TIP]
> Under WSL, libTAS runs the game on the CPU, which is slow (about 12 fps when fast-forwarding). Once the game is set up, follow the [dozen guide](https://github.com/grindersky/UnleashedRecomp/blob/patches/DOZEN.md) to play back and encode movies on your graphics card instead, several times faster.

## Setting Up the TAS Version

### 1. Set up WSL and libTAS

On Windows, follow libTAS's [WSL guide](https://clementgallet.github.io/libTAS/guides/wsl/): it installs WSL 2 with Ubuntu, then libTAS. Everything below runs in that Ubuntu terminal. On Linux, just install libTAS.

The TAS mode was tested with libTAS 1.4.8 on Ubuntu 26.04 in WSL 2.

### 2. Install the build dependencies

```bash
sudo apt install git autoconf automake libtool pkg-config curl cmake ninja-build clang clang-tools libgtk-3-dev
```

### 3. Get the source

Either clone this branch:

```bash
git clone --recurse-submodules -b tas-mode https://github.com/grindersky/UnleashedRecomp.git
cd UnleashedRecomp
```

Or clone the original project and apply the TAS patch from the [`patches` branch](https://github.com/grindersky/UnleashedRecomp/tree/patches):

```bash
git clone --recurse-submodules https://github.com/hedge-dev/UnleashedRecomp.git
cd UnleashedRecomp
curl -LO https://raw.githubusercontent.com/grindersky/UnleashedRecomp/patches/UnleashedTAS.patch
git apply UnleashedTAS.patch
```

> [!NOTE]
> The patch is made against the original project's commit `cf829a9`. If `git apply` fails because the project has changed since, check out that commit first with `git checkout cf829a9 && git submodule update --init --recursive`, then apply the patch.

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
> Software rendering is slow. To play back and encode movies on your graphics card, several times faster, set up dozen with the [dozen guide](https://github.com/grindersky/UnleashedRecomp/blob/patches/DOZEN.md). Keep software rendering on while making TASes: savestates haven't been tested on dozen.

Start the game from libTAS and TAS as usual.

### Updating the TAS version

When the TAS version gets an update, update the source, then build the game again.

If you cloned this branch:

```bash
cd UnleashedRecomp
git pull
git submodule sync --recursive
git submodule update --init --recursive
```

If you applied the patch to the original project, take the old patch back out with the `UnleashedTAS.patch` you applied, then download and apply the new one:

```bash
cd UnleashedRecomp
git apply -R UnleashedTAS.patch
curl -LO https://raw.githubusercontent.com/grindersky/UnleashedRecomp/patches/UnleashedTAS.patch
git apply UnleashedTAS.patch
```

`git apply -R` only works if you haven't changed the source yourself since applying the patch. If it fails, start again from a fresh clone, as in step 3.

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
  - As Sonic: his speed, his movement mode (free 3D, forward, 2D, running on water or along a wall, on ice, a boss fight's special mode, or diving), his state (on the ground or in the air, sliding, stomping, and whether a jump, stomp or quick step pressed on the current frame would work, with countdowns until a stomp is allowed again after a jump or an air boost, and until the next quick step), the stick with the M-Speed zone (in 3D, in any direction), the D-Speed zones (in 2D) and the break-jump zone, and the buttons.
  - As the Werehog: his speed, his state (on the ground or in the air, dashing, attacking, which attack and in how many frames it ends on its own, with and without the stick), whether a guard (LB) pressed on the current frame would work, his Unleash gauge, the stage time, the stick and the buttons.

### Mods and codes

The game loads mods and Hedge Mod Manager codes through two files, which Hedge Mod Manager usually writes. In WSL, you don't need Hedge Mod Manager: write them yourself. Edit them while the game is closed, as it only reads them when it starts.

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

  `AllowCancellingUnleash`, `DisableAutoSaveWarning`, `DisableBoostFilter`, `DisableDLCIcon`, `DisableDPadMovement`, `DisableDWMRoundedCorners`, `DisableLowResolutionFontOnCustomUI`, `EnableEventCollisionDebugView`, `EnableGIMipLevelDebugView`, `EnableObjectCollisionDebugView`, `EnableStageCollisionDebugView`, `FixEggmanlandUsingEventGalleryTransition`, `FixUnleashOutOfControlDrain`, `HomingAttackOnJump`, `HUDToggleKey`, `SaveScoreAtCheckpoints`, `SkipIntroLogos`, `UseAlternateTitle`, `UseArrowsForTimeOfDayTransition`, `UseOfficialTitleOnTitleBar`
- **Collision views**: `EnableStageCollisionDebugView` shows the stage's collision instead of its models, `EnableEventCollisionDebugView` the event triggers, and `EnableObjectCollisionDebugView` the objects' collision. With the stage collision view, the grind rails' collision is drawn too (orange), and with any of the three, the player's collision that's on: his body (cyan) and what touches rings, objects and triggers (magenta). They're hidden behind walls; press F12 to switch to drawing them over everything, walls included, and back. Hiding them needs anti-aliasing off: with it on, they're always drawn over everything. Under libTAS, F12 is recorded into the movie like the game's keys.

### Keeping movies in sync

libTAS doesn't save everything that matters in a movie. Play movies back with:

- the game's FPS option set to libTAS's frame rate,
- the same mods and codes enabled as when the movie was recorded (see [Mods and codes](#mods-and-codes)),
- the same save.

Movies recorded at frame rates other than 60 fps with a version from before 2026-09-28 will probably desync.

### Faster playback and encoding

To play back and encode movies on your graphics card instead of the CPU under WSL, see the [dozen guide](https://github.com/grindersky/UnleashedRecomp/blob/patches/DOZEN.md). It makes playback and encoding several times faster.

