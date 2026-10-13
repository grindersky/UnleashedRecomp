# Unleashed Recompiled TAS mode patch

`UnleashedTAS.patch` adds a TAS mode to [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) for
running under [libTAS](https://github.com/clementgallet/libTAS) on Linux: deterministic movie playback, savestates,
libTAS's Stop button and OSD, correct audio at any libTAS frame rate, and an optional HUD.

For the full setup, from WSL and libTAS to the libTAS settings to use, see the guide in the
[`tas-mode` branch's README](https://github.com/grindersky/UnleashedRecomp/tree/tas-mode#setting-up-the-tas-version).

> [!TIP]
> Under WSL, libTAS runs the game on the CPU, which is slow (about 12 fps when fast-forwarding). Once the game is set
> up, follow [DOZEN.md](DOZEN.md) to play back and encode movies on your graphics card instead, several times faster.

## Applying it

```bash
git clone --recursive https://github.com/hedge-dev/UnleashedRecomp.git
cd UnleashedRecomp
git apply UnleashedTAS.patch
cmake . --preset linux-release
cmake --build ./out/build/linux-release --target UnleashedRecomp -j6
```

Put your own game files in `UnleashedRecompLib/private/` before building, as for a normal build.

## Running it

```bash
UNLEASHED_TAS_MODE=1 libTAS
```

Add `UNLEASHED_TAS_HUD=1` for a HUD, shown in levels. It's drawn into the frames, so it also shows up in encodes.

- As Sonic: his speed, his movement mode (free 3D, forward, 2D, running on water or along a wall, on ice, a boss
  fight's special mode, or diving), his state (on the ground or in the air, sliding, stomping, and whether a jump,
  stomp or quick step pressed on the current frame would work, with countdowns until a stomp is allowed again after a
  jump or an air boost, and until the next quick step), the stick with the M-Speed zone (in 3D, in any direction), the
  D-Speed zones (in 2D) and the break-jump zone (orange), and the buttons (lit while held, with a ring on the frame
  they're pressed).
- As the Werehog: his speed, his state (on the ground or in the air, dashing, attacking, which attack and in how many
  frames it ends on its own, with and without the stick), whether a guard (LB) pressed on the current frame would
  work, his Unleash gauge, the stage time, the stick and the buttons.

The collision view codes (`EnableStageCollisionDebugView`, `EnableEventCollisionDebugView` and
`EnableObjectCollisionDebugView`) also draw the player's collision that's on: his body (cyan), what touches rings,
objects and triggers (magenta), and day Sonic's path collision (blue), which most likely touches paths like the grind
rails. The stage collision view also draws the grind rails' collision (orange). They're
hidden behind walls; F12 switches to drawing them over everything, walls included, and back. Hiding them needs
anti-aliasing off: with it on, they're always drawn over everything. Under libTAS, F12 is recorded into the movie.

## Keeping movies in sync

libTAS doesn't save everything that matters in the movie: play it back with the game's FPS option set to libTAS's
frame rate, and with the same codes enabled as when it was recorded.

Movies recorded at frame rates other than 60 fps with a version of this patch from before 2026-09-28 will probably
desync: their audio played at the wrong speed, and fixing that changed how many frames loads take.

The source, with its history, is on the `tas-mode` branch of this repository.
