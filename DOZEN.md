# Dozen for Unleashed TAS

Under WSL, libTAS normally runs the game on lavapipe, a Vulkan driver that draws everything on the CPU. Dozen is
Mesa's Vulkan driver built on Direct3D 12, so the game draws on your real graphics card instead. That makes playing
back and encoding movies several times faster:

| | |
|---|---|
| Fast-forward playback on dozen | 41.6 fps |
| The same playback on lavapipe | 12.1 fps |
| Encoding 720p with a 4K upscale on dozen | 11–15 fps |

Use dozen to **play back and encode** movies. Keep making TASes (recording, savestates) on lavapipe, by turning on
libTAS's "Force software rendering": savestates haven't been tested on dozen. A movie plays back the same on both
drivers; playbacks on each were compared frame by frame and matched over 2800 frames.

## What you need

- Windows 11, or Windows 10 21H2 or newer, with WSL 2 up to date (run `wsl --update` in PowerShell).
- A current graphics driver from NVIDIA, AMD or Intel installed in Windows.
- Ubuntu in WSL. This guide was tested on Ubuntu 26.04 with Mesa 26.0.8.
- Unleashed Recompiled built with the current `UnleashedTAS.patch` from this branch (2026-09-28 or later), and
  libTAS installed. Older patches don't run on dozen.

Check that WSL can reach your graphics card. Both files must exist:

```bash
ls /usr/lib/wsl/lib/libd3d12.so /usr/lib/wsl/lib/libdxcore.so
```

## Install dozen

Everything is built and installed inside your home folder, in `~/dozen`. Nothing replaces the system's drivers,
and the build takes a few minutes.

### 1. Install the build tools and headers

```bash
sudo apt update
sudo apt install -y build-essential pkg-config meson ninja-build bison flex \
  glslang-tools python3-mako python3-yaml python3-packaging directx-headers-dev \
  libdrm-dev libexpat1-dev zlib1g-dev libzstd-dev libudev-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxshmfence-dev \
  libxcb-dri3-dev libxcb-present-dev libxcb-randr0-dev libxcb-xfixes0-dev \
  libxcb-sync-dev libxcb-shm0-dev vulkan-tools wget
```

### 2. Download Mesa 26.0.8

```bash
mkdir -p ~/dozen && cd ~/dozen
wget https://archive.mesa3d.org/mesa-26.0.8.tar.xz
tar xf mesa-26.0.8.tar.xz
```

### 3. Configure a build with only dozen

This turns off everything else in Mesa (OpenGL, the other drivers), which keeps the build short.

```bash
cd ~/dozen/mesa-26.0.8
meson setup build --prefix="$HOME/dozen/install" -Dbuildtype=release \
  -Dvulkan-drivers=microsoft-experimental -Dgallium-drivers= -Dplatforms=x11 \
  -Dllvm=disabled -Dopengl=false -Degl=disabled -Dglx=disabled -Dgbm=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dtools= -Dvideo-codecs= -Dvulkan-layers=
```

It should end with a summary listing `vulkan-drivers : microsoft-experimental`.

### 4. Build and install

```bash
ninja -C build
ninja -C build install
```

The driver ends up in `~/dozen/install/lib/x86_64-linux-gnu/libvulkan_dzn.so`.

### 5. Check that Vulkan finds your graphics card through dozen

```bash
VK_DRIVER_FILES=$HOME/dozen/install/share/vulkan/icd.d/dzn_icd.x86_64.json vulkaninfo --summary | grep -E "deviceName|driverName"
```

You should see `deviceName = Microsoft Direct3D12 (your graphics card)` and `driverName = Dozen`. A warning that dzn
is not a conformant Vulkan implementation is normal.

### 6. Add commands that start libTAS with dozen

```bash
echo "alias libTASdozen='UNLEASHED_TAS_MODE=1 VK_DRIVER_FILES=$HOME/dozen/install/share/vulkan/icd.d/dzn_icd.x86_64.json libTAS'" >> ~/.bashrc
echo "alias libTASdozenHUD='UNLEASHED_TAS_MODE=1 UNLEASHED_TAS_HUD=1 VK_DRIVER_FILES=$HOME/dozen/install/share/vulkan/icd.d/dzn_icd.x86_64.json libTAS'" >> ~/.bashrc
source ~/.bashrc
```

From now on, `libTASdozen` starts libTAS with the TAS mode on and dozen, and `libTASdozenHUD` does the same with the
TAS HUD. If you added `libTASdozen` from an older version of this guide, the new line replaces it.

### 7. Choose the driver with libTAS's software rendering

With the game closed, open libTAS's **Settings** and go to **Video**. **Force software rendering** picks the driver:

- **Off**: the game draws on dozen. Use this to play back and encode.
- **On**: libTAS switches the game back to lavapipe without saying so. Use this to make TASes.

libTAS remembers the setting, so switch it when you go from making a TAS to encoding and back.

### 8. Play back or encode a movie

```bash
libTASdozen
```

Or `libTASdozenHUD` for the HUD. Load your movie and encode as usual.

To confirm dozen is in use: when the game starts, the terminal you started libTAS from shows
`WARNING: dzn is not a conformant Vulkan implementation, testing use only.`

> Dozen changes only how frames are drawn. The TAS mode makes the game behave the same on both drivers, so movies
> made on lavapipe play back in sync on dozen. Keep the same game FPS option and codes as when the movie was recorded.

## If something goes wrong

**vulkaninfo lists llvmpipe instead of Direct3D12**
The `VK_DRIVER_FILES` path is wrong. Check that `~/dozen/install/share/vulkan/icd.d/dzn_icd.x86_64.json` exists and
that step 4 finished without errors.

**The game still runs slowly and the terminal doesn't show the dzn warning**
"Force software rendering" is still checked in libTAS (step 7), or libTAS was started with `libTAS` instead of
`libTASdozen` or `libTASdozenHUD`.

**`libd3d12.so` is missing**
WSL has no access to your graphics card. Update Windows, run `wsl --update`, install the latest graphics driver,
then restart WSL with `wsl --shutdown`.

**meson can't find DirectX-Headers, or apt has no `directx-headers-dev`**
Leave that package out of step 1. Mesa then downloads the headers by itself while configuring, which needs an
internet connection.

**The game crashes at startup on dozen**
Rebuild Unleashed Recompiled with the current `UnleashedTAS.patch`. Older versions require a Vulkan extension that
dozen doesn't have.

---

Tested on Ubuntu 26.04.1 in WSL 2 with Mesa 26.0.8 (meson 1.10.1, GCC 15.2) and an NVIDIA GeForce RTX 5070. Speeds
are from playing back and encoding the same movie on the same machine.
