#include <kernel/memory.h>
#include <tas_mode.h>

// Patches that only do something in TAS mode (see tas_mode.cpp).

// Not every loop in the audio library needs a yield: the one at 0x8315D8E8 (in sub_8315D800) looks like a
// spin-wait but drains a voice's list of consumed buffers, which always terminates. Yielding there made
// the virtual clock jump to the next audio callback for every entry, running audio ~12x too fast.

// The heads of loops in the audio library (in sub_83148E40 and sub_8314A410) that stop a sound: they update
// the sound's streams, without ever blocking, until it has stopped, which needs the audio thread to run.
void TasStreamStopWaitMidAsmHook()
{
    if (IsTasMode())
        TasScheduler::YieldIfPolling();
}

// A loop in the audio library (in sub_8316CF50) that polls a task's status, without ever blocking, until
// another thread has advanced it to 2. The hook is only reached when the loop comes around again, with the
// status just polled in r3.
void TasSpinYieldMidAsmHook(PPCRegister& r3)
{
    if (IsTasMode() && r3.s32 != 2)
        TasScheduler::SleepFor(0);
}
