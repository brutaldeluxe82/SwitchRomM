#pragma once

// app_shell.hpp — RomApp: Application/stack/shell + shared widget builders.
// Mirrors Grout's gabagool patterns: screen stack with scroll resume,
// options lists with cycling rows, detail screens, footer hints, process
// modals, dialogs, toasts. Coordinates use Plutonium's 1280x720 base grid.

#include "ui_common.hpp"
#include "romm/status.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <set>
#include <vector>

namespace romm::ui {

// Ticks since start (ms); defined in services.cpp.
uint32_t TicksMs();

// One entry of Grout's navigation stack (scroll-resume state).
struct StackEntry {
    int screenId{-1};
    int selectedIndex{0};        // cursor to restore on back
    int visibleStartIndex{0};    // scroll offset to restore on back
};

// Per-screen retained state for scroll resume (Grout's LastSelectedIndex etc.)
struct ScreenState {
    int selectedIndex{0};
    int visibleStartIndex{0};
    // Options-list per-row cycle index memory, keyed by row label.
    std::vector<std::pair<std::string, int>> cycleMemory;
    // Multi-select queueing (Y toggle + A confirm, Grout-style). Set of
    // selected ROM ids for the list that owns this state; cleared on leave.
    std::set<std::string> selectedIds;
    bool multiSelectActive{false};
    // Download Manager highlighted-row memory (scroll resume + delete).
    int managerCursor{0};
    int GetCycleIndex(const std::string& label, int optionsCount) const {
        for(const auto& kv : cycleMemory) {
            if(kv.first == label) {
                return (optionsCount > 0) ? (kv.second % optionsCount) : 0;
            }
        }
        return 0;
    }
    void SetCycleIndex(const std::string& label, int idx) {
        for(auto& kv : cycleMemory) {
            if(kv.first == label) {
                kv.second = idx;
                return;
            }
        }
        cycleMemory.push_back({label, idx});
    }
};

// Shared async services owned by main.cpp (RomServices), exposed to screens.
struct RomServices;

// RomApp — the gabagool-style router. One Plutonium Application + one shared
// Layout whose children are rebuilt on every navigation (screens are stateless
// builders over App/Services state; Grout's Draw(input) -> (output, error)
// equivalent). Retained scroll/cycle state lives in App::states.
struct App {
    // Non-owning handle to the owning pu::ui::Application (set in
    // RomApp::OnLoad from `this`). MUST be raw: a shared_ptr Ref here was
    // never assigned -> null deref in ShowToast/ShowDialog = every
    // pairing/download/sign-out crash.
    pu::ui::Application* app{nullptr};
    pu::ui::Layout::Ref layout;

    // Navigation stack: back of vector = current screen.
    std::vector<StackEntry> stack;
    std::vector<ScreenState> states; // parallel to stack

    RomServices* services{nullptr};
    std::string title;                       // small title convention
    std::vector<std::string> footerHints;    // {"A Select", "B Back", ...}
    std::string statusLine;                  // transient line above footer
    // --- Navigation API (Grout router semantics) ---
    // push/pop only mutate the stack and set navPending; the render callback
    // applies the rebuild via onNavigate BEFORE this frame's element input.
    bool dialogActive = false; // true while a modal dialog's nested render loop runs.
    // A rebuild replaces the layout's std::function members (and clears
    // elements). Doing that synchronously inside a layout input callback,
    // menu item callback, or DURING a dialog's nested render loop frees the
    // closure currently executing -> use-after-free. Deferring unwinds the
    // handler first; freezing during the dialog skips the nested window.
    std::function<void()> onNavigate;
    bool navPending{false};
    void pushScreen(int screenId);
    void popScreen();
    void RebuildCurrent() { navPending = true; }
    bool stackEmpty() const { return stack.empty(); }
    StackEntry& top() { return stack.back(); }
    ScreenState& state() { return states.back(); }
    // Deferred navigation: unwinds the stack to `targetScreenId` (pushing it
    // if absent) after `delayMs` from `navAtMs`. Driven by the render
    // callback via TickDeferredNav — never fire from input callbacks.
    bool pendingNav = false;
    int targetScreenId = -1;
    uint64_t navAtMs = 0;
    uint64_t delayMs = 0;
    void QueueNavAfter(int screenId, uint64_t delay_ms) {
        pendingNav = true;
        targetScreenId = screenId;
        delayMs = delay_ms;
        navAtMs = services ? TicksMs() : 0;
    }
    // Returns true when the deferred nav fires this frame (render callback
    // then applies stack changes + rebuild).
    bool TickDeferredNav() {
        if (!pendingNav) return false;
        if (TicksMs() - navAtMs < delayMs) return false;
        pendingNav = false;
        if (targetScreenId < 0) return false;
        bool found = false;
        for (const auto& e : stack) {
            if (e.screenId == targetScreenId) { found = true; break; }
        }
        if (found) {
            // Unwind to the target (it becomes the top).
            while (!stack.empty() && stack.back().screenId != targetScreenId) {
                stack.pop_back();
                states.pop_back();
            }
        } else {
            // Not in the stack: replace everything above the root with it.
            while (stack.size() > 1) {
                stack.pop_back();
                states.pop_back();
            }
            stack.push_back({targetScreenId, 0, 0});
            states.push_back({});
        }
        navPending = true;
        return true;
    }

    // Shell helpers
    // Toasts are DEFERRED: StartOverlayWithTimeout installed an overlay that
    // renders during the NEXT frame, but calling it from inside layout input
    // dispatch (or a dialog epilogue) raced the in-flight frame's element
    // sweep and crashed the app (pairing-success + download + sign-out
    // crashes). queueToast stages the text; the render callback shows at most
    // one per frame when no other overlay is active.
    void queueToast(const std::string& text) { pendingToasts.push_back(text); }
    void ShowToast(const std::string& text); // direct: ONLY from render callback context
    void DrainPendingToast();                // called from the render callback
    s32 ShowDialog(const std::string& title, const std::string& content,
                   const std::vector<std::string>& opts, bool lastIsCancel = true);
    void ShowProcess(const std::string& text, std::function<bool()> cancelFn = nullptr); // modal-ish
    void HideProcess();
    std::vector<std::string> pendingToasts;
};

// --- Shared widget builders ---

// Options-list row: label + cycle value (Left/Right cycles, A opens picker).
struct OptionRow {
    std::string label;
    std::vector<std::string> options;          // empty => action-only row
    int current{0};                            // current cycle index
    std::function<void()> onSelect;            // A / click (action rows)
    std::function<void(int)> onCycle;          // after each cycle change
    bool visible{true};
    bool enabled{true};
};

// Shared menu geometry (used by screen files).
inline constexpr s32 kMenuItemH = 135; // 90 * 1.5
inline constexpr s32 kMenuX = kMargin;
inline constexpr s32 kMenuY = 150;     // title at 36, tight gap to list (user ask)
// Menu width leaves room for Plutonium's 30px scrollbar flush against the
// right screen edge (scrollbar renders at menu x + w - 30).
inline constexpr s32 kScrollbarW = 30;
inline constexpr s32 kMenuW = kScreenWidth - kMargin - kScrollbarW;
// Items that fit between kMenuY and the footer without overflowing:
// (984-150)/135 = 6.17 -> 6.
inline constexpr u32 kMenuVisibleItems =
    static_cast<u32>((kFooterY - kMenuY) / kMenuItemH);

using MenuRef = pu::ui::elm::Menu::Ref;
using TextRef = pu::ui::elm::TextBlock::Ref;
using ImageRef = pu::ui::elm::Image::Ref;
using RectRef = pu::ui::elm::Rectangle::Ref;
using ProgressRef = pu::ui::elm::ProgressBar::Ref;

// Add a TextBlock to a layout with standard font/color.
TextRef AddText(pu::ui::Layout::Ref& layout, const std::string& text,
                s32 x, s32 y, const pu::ui::Color& clr = kText,
                const std::string& font = kFontM, s32 maxW = 0);

// Footer hint bar (gabagool footer conventions).
void BuildFooter(App& a);
// Title block (small title convention).
TextRef BuildTitle(App& a, const std::string& text, s32 y = 45); // 30 * 1.5
// Status bar (top strip: clock right-aligned; accent underline).
void BuildStatusBar(App& a);

// Menu factory with gabagool-like geometry (rows, scrollbar, focus colors).
MenuRef MakeMenu(s32 x, s32 y, s32 w, s32 itemH, u32 showCount);
// Word-wrap + vertical auto-scroll text for fixed viewports (game details
// description). Wraps at build time against the element font; scrolls
// downward after a 3-second pause, then loops back to the top.
class ScrollingText : public pu::ui::elm::Element {
    public:
        static constexpr u64 kPauseMs = 3000;   // pause before scrolling
        static constexpr s32 kLineGap = 10;

        ScrollingText(s32 x, s32 y, s32 w, s32 h, const std::string& text,
                      const std::string& font_name, pu::ui::Color clr);
        PU_SMART_CTOR(ScrollingText)
        ~ScrollingText() override;

        inline s32 GetX() override { return x_; }
        inline s32 GetY() override { return y_; }
        inline s32 GetWidth() override { return w_; }
        inline s32 GetHeight() override { return h_; }
        void OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) override;
        void OnInput(const u64, const u64, const u64, pu::ui::TouchPoint) override {}

    private:
        s32 x_, y_, w_, h_;
        std::string font_name_;
        pu::ui::Color clr_;
        std::vector<std::string> lines_;
        std::vector<pu::sdl2::Texture> line_texs_;
        u64 start_ms_ = 0;
        s32 scroll_y_ = 0;
};

// Factory: wraps `text` into a ScrollingText viewport and adds it to the
// layout. Viewport height constant lives here for reuse.
inline constexpr s32 kDescViewportH = 560;
void AddScrollingText(pu::ui::Layout::Ref& layout, const std::string& text,
                      s32 x, s32 y, s32 w, s32 h);

// QR code renderer (nayuki/QR-Code-generator, vendored in
// third_party/qrcodegen). Rasterizes once at build time into a texture and
// draws it scaled with a quiet-zone margin.
class QrCodeView : public pu::ui::elm::Element {
    public:
        QrCodeView(s32 x, s32 y, s32 size, const std::string& data);
        PU_SMART_CTOR(QrCodeView)
        ~QrCodeView() override;

        inline s32 GetX() override { return x_; }
        inline s32 GetY() override { return y_; }
        inline s32 GetWidth() override { return size_; }
        inline s32 GetHeight() override { return size_; }
        inline bool IsValid() const { return tex_ != nullptr; }
        void OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) override;
        void OnInput(const u64, const u64, const u64, pu::ui::TouchPoint) override {}

    private:
        s32 x_, y_, size_;
        pu::sdl2::Texture tex_ = nullptr;
};

// Factory: encodes `data` (URL) into a QR element and adds it to the layout.
void AddQrCode(pu::ui::Layout::Ref& layout, const std::string& data,
               s32 x, s32 y, s32 size);

// Rebuild an options screen: title + menu of OptionRows + footer.
// onBack: invoked on B (footer button of the menu).
void BuildOptionsScreen(App& a, const std::string& title,
                        const std::vector<OptionRow>& rows,
                        std::function<void()> onBack,
                        const std::vector<std::string>& footerHints = {});

// List screen scaffold (Grout lists): title + menu + footer.
// onSelectionChanged fires when the cursor moves (for preview panes).
MenuRef BuildListScreen(App& a, const std::string& title,
                        const std::vector<std::string>& items,
                        std::function<void(int)> onActivate,
                        std::function<void()> onBack,
                        const std::vector<std::string>& footerHints = {});

} // namespace romm::ui
