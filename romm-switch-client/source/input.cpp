#include "romm/input.hpp"
#include "romm/logger.hpp"

namespace romm {

Action translateEvent(const SDL_Event& e) {
    if (e.type == SDL_QUIT) return Action::Quit;
    // Use SDL controller events (Nintendo layout) and ignore raw joystick duplicates.
    if (e.type == SDL_JOYBUTTONDOWN) {
        romm::logDebug("Ignoring SDL_JOYBUTTONDOWN code=" + std::to_string(e.jbutton.button), "INPUT");
        return Action::None;
    }
    // Map controller buttons to Actions (Switch/SDL positional codes):
    // Physical Nintendo labels:
    // - B (bottom) -> back
    // - A (right)  -> select
    // - Y (left)   -> queue
    // - X (top)    -> search (index views) / start downloads (queue)
    // - Minus      -> quit
    // - Plus       -> settings
    // - ZR (right shoulder)  -> cycle PLATFORMS index ROM <-> BIOS
    // - ZL (left shoulder)   -> cycle BIOS <-> ROM
    // - R (right stick click)-> diagnostics
    // - L (left stick click) -> updater
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        static Uint32 lastTicks[SDL_CONTROLLER_BUTTON_MAX] = {};
        Uint32 now = SDL_GetTicks();
        uint8_t code = e.cbutton.button;
        romm::logDebug("SDL controller button pressed code=" + std::to_string(code), "INPUT");
        if (code < SDL_CONTROLLER_BUTTON_MAX) {
            if (now - lastTicks[code] < 40) { // light debounce for double-fires
                romm::logDebug("Debounced duplicate controller code=" + std::to_string(code), "INPUT");
                return Action::None;
            }
            lastTicks[code] = now;
        }
        Action act = Action::None;
        switch (code) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: act = Action::Up; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: act = Action::Down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: act = Action::Left; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: act = Action::Right; break;
            // Map based on SDL positional codes so on-screen Nintendo labels match physical buttons.
            case SDL_CONTROLLER_BUTTON_A: act = Action::Back; break;            // bottom (B) -> back
            case SDL_CONTROLLER_BUTTON_B: act = Action::Select; break;          // right (A) -> select/confirm
            case SDL_CONTROLLER_BUTTON_X: act = Action::OpenQueue; break;       // left (Y) -> queue view
            case SDL_CONTROLLER_BUTTON_Y: act = Action::StartDownload; break;   // top (X) -> start downloads
            case SDL_CONTROLLER_BUTTON_BACK: act = Action::Quit; break;         // Minus -> quit
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: act = Action::CycleIndexForward; break;  // physical L/R (SDL b6/b7)
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: act = Action::CycleIndexBackward; break;  // physical L/R (SDL b6/b7)
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: act = Action::OpenDiagnostics; break;       // R stick
            case SDL_CONTROLLER_BUTTON_LEFTSTICK: act = Action::OpenUpdater; break;            // L stick
            case SDL_CONTROLLER_BUTTON_START: act = Action::OpenSettings; break; // Plus -> settings
            default: break;
        }
        if (act != Action::None) {
            romm::logDebug("Mapped controller code " + std::to_string(code) +
                           " to action " + std::to_string(static_cast<int>(act)),
                           "INPUT");
        }
        return act;
    }
    // Physical ZL/ZR bind as analog triggers in the SDL switch port
    // (mapping: lefttrigger:b8, righttrigger:b9), surfacing as axis motion.
    if (e.type == SDL_CONTROLLERAXISMOTION &&
        (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
         e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
        const bool pressed = e.caxis.value > 8192;
        static bool triggerHeld[2] = {false, false};
        const int idx = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : 1;
        if (pressed && !triggerHeld[idx]) {
            triggerHeld[idx] = true; // fire once per pull; reset on release
            return idx == 0 ? Action::CycleIndexBackward : Action::CycleIndexForward;
        }
        if (!pressed) triggerHeld[idx] = false;
    }
    return Action::None;
}

} // namespace romm
