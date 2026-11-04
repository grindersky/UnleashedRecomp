#!/bin/sh
# kumquat, libTAS's GPU host for "Forwarded GPU" rendering, drawing on dozen (the graphics card under WSL).
# libTAS starts it with the Vulkan driver variables cleared, so they are set here. Lives in the folder the
# Forwarded GPU guide (FORWARDED_GPU.md) builds everything into.
GPU="$(cd "$(dirname "$0")" && pwd)"

export VK_DRIVER_FILES="$GPU/dozen/share/vulkan/icd.d/dzn_icd.x86_64.json"

# The patched dozen: keep compiled pipelines when gfxstream rebuilds the Vulkan device on a state load, and
# reuse the GPU memory the load frees. The compiled shaders also go to a disk cache.
export DZN_PERSISTENT_PSO_CACHE=1
export DZN_RECYCLE=1
export MESA_SHADER_CACHE_DIR="$GPU/shader-cache"

export LD_LIBRARY_PATH="$GPU/prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$GPU/src/rutabaga_gfx/target/release/kumquat" "$@"
