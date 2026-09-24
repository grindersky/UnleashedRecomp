#include "hid.h"
#include <ui/game_window.h>
#include <tas_mode.h>
#include <user/config.h>

hid::EInputDevice hid::g_inputDevice;
hid::EInputDevice hid::g_inputDeviceController;
hid::EInputDeviceExplicit hid::g_inputDeviceExplicit;

uint16_t hid::g_prohibitedButtons;
bool hid::g_isLeftStickProhibited;
bool hid::g_isRightStickProhibited;

void hid::SetProhibitedInputs(uint16_t wButtons, bool leftStick, bool rightStick)
{
    hid::g_prohibitedButtons = wButtons;
    hid::g_isLeftStickProhibited = leftStick;
    hid::g_isRightStickProhibited = rightStick;
}

bool hid::IsInputAllowed()
{
    // In TAS mode, inputs come from libTAS regardless of which window has focus.
    return GameWindow::s_isFocused || Config::AllowBackgroundInput || IsTasMode();
}

bool hid::IsInputDeviceController()
{
    return hid::g_inputDevice != hid::EInputDevice::Keyboard &&
        hid::g_inputDevice != hid::EInputDevice::Mouse;
}
