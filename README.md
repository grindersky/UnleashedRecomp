# Unleashed Recompiled TAS mode patch

`UnleashedTAS.patch` adds a TAS mode to [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) for
running under [libTAS](https://github.com/clementgallet/libTAS) on Linux: deterministic movie playback, savestates,
libTAS's Stop button and OSD, correct audio at any libTAS frame rate, and an optional HUD.

For the full setup, from WSL and libTAS to the libTAS settings to use, see the guide in the
[`tas-mode` branch's README](https://github.com/grindersky/UnleashedRecomp/tree/tas-mode#setting-up-the-tas-version).

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

- As Sonic: his speed, his state (on the ground or in the air, sliding, stomping, and whether a jump or stomp pressed
  on the current frame would work, with a countdown until a stomp is allowed again after a jump), the stick with the
  M-/D-Speed zones and the break-jump zone (orange), and the buttons (lit while held, with a ring on the frame they're
  pressed).
- As the Werehog: his speed, his state (on the ground or in the air, dashing, attacking, which attack and in how many
  frames it ends on its own, with and without the stick), whether a guard (LB) pressed on the current frame would
  work, his Unleash gauge, the stage time, the stick and the buttons.

To play back and encode movies on your graphics card instead of the CPU under WSL, see [DOZEN.md](DOZEN.md).

## Keeping movies in sync

libTAS doesn't save everything that matters in the movie: play it back with the game's FPS option set to libTAS's
frame rate, and with the same codes enabled as when it was recorded.

Movies recorded at frame rates other than 60 fps with a version of this patch from before 2026-09-28 will probably
desync: their audio played at the wrong speed, and fixing that changed how many frames loads take.

The source, with its history, is on the `tas-mode` branch of this repository.
