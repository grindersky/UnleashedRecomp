#!/bin/bash
# libTAS with "Forwarded GPU" rendering (savestates on the graphics card) for Unleashed Recompiled, with the
# TAS mode on. Lives in the folder the Forwarded GPU guide (FORWARDED_GPU.md) builds everything into.
# Usage: libTASGPU.sh [--hud] [libTAS arguments]
GPU="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = --hud ]; then export UNLEASHED_TAS_HUD=1; shift; fi
export UNLEASHED_TAS_MODE=1

# WSL can't share GPU memory with the game (no udmabuf), so kumquat copies it.
export LIBTAS_GPU_MODE=COPYMODE
export LIBTAS_KUMQUAT_BIN="$GPU/kumquat.sh"
export LIBTAS_GFXSTREAM_ICD="$GPU/prefix/share/vulkan/icd.d/gfxstream_vk_icd.x86_64.json"

# For libTAS's "Native" rendering: the game draws on dozen directly (fast, but savestates crash the game).
export VK_DRIVER_FILES="$GPU/dozen/share/vulkan/icd.d/dzn_icd.x86_64.json"

exec "$GPU/bin/libTAS" "$@"
