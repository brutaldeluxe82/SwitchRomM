#pragma once

#include <SDL2/SDL.h>

namespace romm {

enum class Action {
    None,
    Up,
    Down,
    Left,
    Right,
    Select,
    OpenDiagnostics,
    OpenUpdater,
    OpenSettings, // Plus: open settings
    CycleIndexForward,  // ZR: PLATFORMS ROM <-> BIOS index
    CycleIndexBackward, // ZL: BIOS <-> ROM
    OpenQueue,
    Back,
    StartDownload,
    Quit
};

Action translateEvent(const SDL_Event& e);

} // namespace romm
