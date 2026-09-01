#pragma once

// ui_common.hpp — Grout parity UI: theme, fonts, shared element helpers.
// Colors/typography mirror gabagool's theming with Grout's defaults:
//   accent #007C77, text white, background white, highlight white.
// Coordinates use Plutonium's 1280x720 base grid (x1.5 -> 1920x1080).

#include <pu/ui/ui_Application.hpp>
#include <pu/ui/ui_Layout.hpp>
#include <pu/ui/ui_Dialog.hpp>
#include <pu/ui/extras/extras_Toast.hpp>
#include <pu/ui/elm/elm_Menu.hpp>
#include <pu/ui/elm/elm_TextBlock.hpp>
#include <pu/ui/elm/elm_Image.hpp>
#include <pu/ui/elm/elm_Rectangle.hpp>
#include <pu/ui/elm/elm_ProgressBar.hpp>
#include <pu/ui/elm/elm_Button.hpp>
#include <pu/ui/elm/elm_Toggle.hpp>

#include <functional>
#include <string>
#include <vector>

namespace romm::ui {


// ---- Theme (gabagool defaults + Grout accent) ----
inline constexpr pu::ui::Color kAccent{0x00, 0x7C, 0x77, 0xFF};   // #007C77
inline constexpr pu::ui::Color kText{0xFF, 0xFF, 0xFF, 0xFF};     // white
inline constexpr pu::ui::Color kHighlightedText{0x00, 0x00, 0x00, 0xFF}; // black
inline constexpr pu::ui::Color kBackground{0xFF, 0xFF, 0xFF, 0xFF}; // white
inline constexpr pu::ui::Color kDim{0xE6, 0xE6, 0xE6, 0xFF};      // subtle row
inline constexpr pu::ui::Color kWhite{0xFF, 0xFF, 0xFF, 0xFF};
inline constexpr pu::ui::Color kHint{0x00, 0x00, 0x00, 0xFF};     // footer/hint text (black on white)

inline const std::string kFont = pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Large);        // 45
inline const std::string kFontM = pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::MediumLarge); // 37
inline const std::string kFontS = pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Medium);      // 30
inline const std::string kFontXS = pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small);      // 27

// ---- Layout metrics (gabagool-style) ----
// Coordinates are laid out on Plutonium's NATIVE 1920x1080 framebuffer.
// The old 1280x720 grid left the UI in the top-left quarter of the screen:
// Plutonium scales touch input by ScreenFactor (1.5) but NOT element
// coordinates, so apps must draw at native resolution themselves.
inline constexpr s32 kScreenWidth = 1920;
inline constexpr s32 kScreenHeight = 1080;
inline constexpr s32 kMargin = 90;   // 60 * 1.5
// Footer sits higher for breathing room above the screen edge; the gap under
// the last list row is the user-tuned "padding between lists and footer".
inline constexpr s32 kFooterY = kScreenHeight - 96;   // 984; ~26px gap below footer text
inline constexpr s32 kStatusBarH = 60;
} // namespace romm::ui
