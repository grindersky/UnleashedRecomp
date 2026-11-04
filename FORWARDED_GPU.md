# Forwarded GPU: savestates on the graphics card

libTAS normally saves a state by copying the game's process. A graphics card's state lives outside the process, in
the driver, so savestates only work when the game draws on the CPU (lavapipe, "Force software rendering"), which is
slow. On dozen (see the [dozen guide](DOZEN.md)) the game is fast, but loading a savestate crashes it.

**Forwarded GPU** is a rendering mode in development in [jakobhellermann's libTAS](https://github.com/jakobhellermann/libTAS).
The game draws through a driver that sends everything to a separate process, **kumquat**, which draws on your
graphics card with gfxstream and can save and restore the whole GPU state along with the game. With it, you can
make TASes on your graphics card, savestates included.

| On an RTX 5070, Rooftop Run | |
|---|---|
| Saving a state | about 0.9 s |
| Loading a state | about 0.7-0.9 s |
| GPU state per savestate slot | about 140 MB |

> [!WARNING]
> This isn't part of a libTAS release yet. The guide builds pinned versions of jakob's libTAS, gfxstream, Mesa and
> kumquat, with fixes for WSL and faster savestates on top (in the `unleashed` branches of the
> [grindersky](https://github.com/grindersky) forks), and a patched dozen. Expect rough edges. Movies stay
> compatible: the TAS mode plays them back the same with any renderer.

This guide is for WSL 2 on Windows, where the GPU host draws on dozen.

## What you need

- The TAS version of Unleashed Recompiled set up as in the README, from 2026-11-04 or later. Older versions show a
  black picture after loading a state, or shadow stripes on models.
- WSL 2 with access to your graphics card: both files below must exist (see the [dozen guide](DOZEN.md)'s
  requirements).

  ```bash
  ls /usr/lib/wsl/lib/libd3d12.so /usr/lib/wsl/lib/libdxcore.so
  ```

- About 15 GB of free space in WSL for the sources and builds, and an hour or so for building.
- Enough memory for the savestates: the GPU part of each slot you use stays in `/tmp`, which WSL keeps in memory.

Everything is built into one folder, `~/libtas-gpu`. Nothing replaces your system's libTAS or drivers: the
`libTAS` you already have keeps working as before.

## 1. Install the build tools

```bash
sudo apt update
sudo apt install -y build-essential git curl wget pkg-config cmake meson ninja-build bison flex \
  glslang-tools python3-mako python3-yaml python3-packaging directx-headers-dev protobuf-compiler \
  libdrm-dev libexpat1-dev zlib1g-dev libzstd-dev libudev-dev libclang-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxshmfence-dev \
  libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-randr0-dev libxcb-xfixes0-dev \
  libxcb-sync-dev libxcb-shm0-dev libxcb-keysyms1-dev libxcb-xkb-dev \
  automake qtbase5-dev liblua5.4-dev libasound2-dev libavutil-dev libswresample-dev \
  libswscale-dev ffmpeg libcap-dev vulkan-tools
```

Mesa and kumquat are partly written in Rust. Install it with rustup, then bindgen, which Mesa needs:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source ~/.cargo/env
cargo install bindgen-cli
```

## 2. Get the sources

Set two folders for the commands below: where to build, and where your TAS version of Unleashed Recompiled is (the
folder you cloned in the README's step 3). Run these again if you open a new terminal halfway through.

```bash
export GPU=~/libtas-gpu
export TAS=~/UnleashedRecomp
```

```bash
mkdir -p $GPU/src && cd $GPU/src
git clone --depth 1 -b unleashed https://github.com/grindersky/libTAS.git
git clone --depth 1 -b unleashed https://github.com/grindersky/gfxstream.git
git clone --depth 1 -b unleashed https://github.com/grindersky/mesa.git
git clone --depth 1 -b unleashed https://github.com/grindersky/rutabaga_gfx.git
wget https://archive.mesa3d.org/mesa-26.0.8.tar.xz
tar xf mesa-26.0.8.tar.xz
```

## 3. Build gfxstream

gfxstream does the GPU work in kumquat, and saves and restores it. `-Dsnapshot=enabled` is what makes savestates
possible.

```bash
cd $GPU/src/gfxstream
meson setup build --prefix=$GPU/prefix --libdir=lib -Dbuildtype=release \
  -Dgfxstream-build=host -Dsnapshot=enabled -Dcpp_std=gnu++20
ninja -C build install
```

## 4. Build the game's side of the driver

This is the Vulkan driver the game loads: it sends the game's GPU work to kumquat.

```bash
cd $GPU/src/mesa
meson setup build --prefix=$GPU/prefix --libdir=lib -Dbuildtype=release -Dauto_features=disabled \
  -Dplatforms=x11 -Dvulkan-drivers=gfxstream -Dgallium-drivers= -Dvirtgpu_kumquat=true \
  -Dopengl=false -Dllvm=disabled -Dgbm=disabled -Dglvnd=disabled -Degl=disabled -Dglx=disabled \
  -Dbuild-tests=false
ninja -C build install
```

## 5. Build kumquat

```bash
cd $GPU/src/rutabaga_gfx
PKG_CONFIG_PATH=$GPU/prefix/lib/pkgconfig cargo build --release -p kumquat_virtio --features gfxstream
```

The first `cargo` run also installs the Rust version this project asks for.

## 6. Build the patched dozen

kumquat draws on dozen. Two patches from the TAS version make savestate loads several times faster: one keeps
dozen's compiled pipelines when gfxstream rebuilds the GPU device on a load, the other reuses the GPU memory the
load frees.

```bash
cd $GPU/src/mesa-26.0.8
patch -p1 < $TAS/forwarded-gpu/dozen-1-pipeline-cache.patch
patch -p1 < $TAS/forwarded-gpu/dozen-2-recycle.patch
meson setup build --prefix=$GPU/dozen -Dbuildtype=release \
  -Dvulkan-drivers=microsoft-experimental -Dgallium-drivers= -Dplatforms=x11 \
  -Dllvm=disabled -Dopengl=false -Degl=disabled -Dglx=disabled -Dgbm=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dtools= -Dvideo-codecs= -Dvulkan-layers=
ninja -C build install
```

If you already followed the [dozen guide](DOZEN.md), this is a second copy: the one in `~/dozen` stays as it is.

## 7. Build libTAS

```bash
cd $GPU/src/libTAS
aclocal && autoconf && autoheader && automake --add-missing
mkdir -p build && cd build
../configure
make -j$(nproc)
mkdir -p $GPU/bin
cp src/program/libTAS src/library/libtas.so $GPU/bin/
```

## 8. Add the launchers

```bash
cp $TAS/forwarded-gpu/kumquat.sh $TAS/forwarded-gpu/libTASGPU.sh $GPU/
chmod +x $GPU/kumquat.sh $GPU/libTASGPU.sh
mkdir -p $GPU/shader-cache
echo "alias libTASGPU='$GPU/libTASGPU.sh'" >> ~/.bashrc
echo "alias libTASGPUHUD='$GPU/libTASGPU.sh --hud'" >> ~/.bashrc
source ~/.bashrc
```

From now on, `libTASGPU` starts this libTAS with the TAS mode on, and `libTASGPUHUD` does the same with the TAS
HUD.

## 9. Set up libTAS

```bash
libTASGPU
```

Choose the game's executable. libTAS keeps its settings per game, so the ones from the README's step 7 carry over
if you set them up already; otherwise set them now. Then, with the game closed, open **Settings → Video** and set
**Rendering** to **Forwarded GPU**. (The `libTAS` from the README ignores this setting and keeps using "Force
software rendering".)

Start the game and TAS as usual: savestates now work on your graphics card.

- The first load in a level takes longer while dozen compiles the level's shaders. They go to a disk cache in
  `~/libtas-gpu/shader-cache`, so it's faster after that.
- Rendering **Native** draws on dozen directly (fast, but savestates crash the game) and **Software** on lavapipe.
  Movies play back the same in all three.

## If something goes wrong

kumquat writes what it does to `/tmp/libtas-gpu/kumquat.log`; check it first.

**The game crashes at startup with `Failed to allocate coherent memory`**
libTAS was started without `LIBTAS_GPU_MODE=COPYMODE`: use `libTASGPU` or `libTASGPUHUD`, not `libTAS`.

**`FATAL GPU host: kumquat did not answer in time`**
A save or load took longer than libTAS waits (120 s). Raise it, for example
`LIBTAS_GPU_TIMEOUT_SECS=300 libTASGPU`, and check that WSL still has free memory (`free -h`).

**The picture goes black after loading a state, or models shadow themselves in stripes**
Update the TAS version and rebuild it (README, "Updating the TAS version").

**Loads take several seconds**
Check that `~/libtas-gpu/kumquat.sh` sets `DZN_PERSISTENT_PSO_CACHE=1` and `DZN_RECYCLE=1`, and that step 6 applied
both patches without errors.

**WSL runs out of memory with many savestates**
Every slot you save keeps its GPU part in `/tmp` until libTAS closes. Use fewer slots, or give WSL more memory in
`%UserProfile%\.wslconfig` (`[wsl2]`, `memory=16GB`), then run `wsl --shutdown`.

---

Tested on Ubuntu 26.04.1 in WSL 2 with an NVIDIA GeForce RTX 5070. Pinned versions (the `unleashed` branches):
libTAS from jakobhellermann/libTAS dev with a longer GPU host timeout and SDL fixes; gfxstream from
jakobhellermann/gfxstream dev with batched snapshot transfers and zstd-compressed snapshots; Mesa
(gfxstream guest driver) and rutabaga_gfx (kumquat) from jakobhellermann's dev branches.
