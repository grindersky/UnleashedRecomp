# Unleashed Recompiled TAS mode patch

`UnleashedTAS.patch` adds a TAS mode to [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) for
running under [libTAS](https://github.com/clementgallet/libTAS) on Linux: deterministic movie playback, savestates,
libTAS's Stop button and OSD, and an optional speedometer and M-/D-Speed stick overlay.

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

Add `UNLEASHED_TAS_HUD=1` for the speedometer and stick overlay (it's drawn into the frames, so it also shows up
in encodes).

The source, with its history, is on the `tas-mode` branch of this repository.
