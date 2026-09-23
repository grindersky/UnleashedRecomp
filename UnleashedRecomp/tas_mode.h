#pragma once

#include <cstdlib>
#include <cstring>

// TAS mode is enabled by setting UNLEASHED_TAS_MODE=1 in the environment.
// It adjusts behaviour that breaks under libTAS (frozen fake time, savestates),
// while leaving the normal game untouched.
inline bool IsTasMode()
{
    static const bool s_tasMode = []()
    {
        const char* value = std::getenv("UNLEASHED_TAS_MODE");
        return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != '\0';
    }();

    return s_tasMode;
}

// Makes the guest timebase (mftb) follow std::chrono::steady_clock, which libTAS controls,
// instead of the CPU's time stamp counter. Must be called before guest code runs.
void InstallTasTimebase();

// Starts the thread that creates the Vulkan driver's threads on its behalf (see tas_mode.cpp).
// Must be called before the Vulkan device is created.
void StartTasThreadSpawner();
