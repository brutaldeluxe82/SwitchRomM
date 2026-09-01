// app_shell.cpp — RomApp implementation: shell, footer, status bar, options
// and list scaffolds, dialogs, toasts, process modal.

#include "app_shell.hpp"

#include <switch.h>
#include <qrcodegen.h>
#include "romm/logger.hpp"
#include <ctime>

namespace romm::ui {

namespace {


std::string ClockText() {
    time_t now = time(nullptr);
    struct tm tmNow{};
    localtime_r(&now, &tmNow);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
    return buf;
}

} // namespace

// --- Navigation ---

void App::pushScreen(int screenId) {
    stack.push_back({screenId, 0, 0});
    states.push_back({});
    navPending = true; // rebuild happens in the render callback (see header)
}

void App::popScreen() {
    if(!stack.empty()) {
        stack.pop_back();
        states.pop_back();
        navPending = true;
    }
}

// --- Shell helpers ---

void App::ShowToast(const std::string& text) {
    if (!this->app) {
        romm::logLine("UI BUG: ShowToast before shell.app wired (dropped: " + text + ")");
        return;
    }
    auto tb = pu::ui::elm::TextBlock::New(0, 0, text);
    tb->SetFont(kFontS);
    tb->SetColor(kHint);
    auto toast = pu::ui::extras::Toast::New(tb, kWhite);
    toast->SetY(900);
    this->app->StartOverlayWithTimeout(toast, 2000);
}

void App::DrainPendingToast() {
    // Called from the render callback ONLY, when no overlay is up. Showing
    // one toast per frame keeps the queue draining without stacking overlays.
    if (this->pendingToasts.empty()) return;
    std::string text = this->pendingToasts.front();
    this->pendingToasts.erase(this->pendingToasts.begin());
    this->ShowToast(text);
}

s32 App::ShowDialog(const std::string& title, const std::string& content,
                    const std::vector<std::string>& opts, bool lastIsCancel) {
    if (!this->app) {
        romm::logLine("UI BUG: ShowDialog before shell.app wired (dropped: " + title + ")");
        return -1;
    }
    // Plutonium dialogs run a nested render loop; the render callback is
    // frozen for their duration (main.cpp) so no rebuild can free the running
    // input handler.
    this->dialogActive = true;
    const s32 choice = this->app->CreateShowDialog(title, content, opts, lastIsCancel);
    this->dialogActive = false;
    return choice;
}
void App::ShowProcess(const std::string& text, std::function<bool()> cancelFn) {
    if (!this->app) {
        romm::logLine("UI BUG: ShowProcess before shell.app wired (dropped: " + text + ")");
        return;
    }
    auto ovl = pu::ui::Overlay::New(210, 450, kScreenWidth - 420, 180, kBackground);
    ovl->SetRadius(20);
    auto tb = pu::ui::elm::TextBlock::New(40, 45, text);
    tb->SetFont(kFontM);
    tb->SetColor(kHint);
    ovl->Add(tb);
    (void)cancelFn;
    this->app->StartOverlay(ovl);
}

void App::HideProcess() {
    this->app->EndOverlay();
}
TextRef AddText(pu::ui::Layout::Ref& layout, const std::string& text,
                s32 x, s32 y, const pu::ui::Color& clr, const std::string& font, s32 maxW) {
    auto tb = pu::ui::elm::TextBlock::New(x, y, text);
    tb->SetFont(font);
    tb->SetColor(clr);
    if(maxW > 0) {
        tb->SetClampWidth(maxW);
    }
    layout->Add(tb);
    return tb;
}

void BuildStatusBar(App& a) {
    // Top strip: battery + 24h clock, right-aligned a few px from the edge
    // (Grout's status bar). psm is initialized in main(); charge is cheap to
    // read per rebuild.
    u32 chargePct = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetBatteryChargePercentage(&chargePct))) chargePct = 0;
    (void)psmGetChargerType(&charger);
    const bool charging = (charger == PsmChargerType_EnoughPower ||
                           charger == PsmChargerType_LowPower);
    char buf[24];
    time_t now = time(nullptr);
    struct tm tmNow{};
    localtime_r(&now, &tmNow);
    snprintf(buf, sizeof(buf), "%s%u%%  %02d:%02d", charging ? "+" : "",
             chargePct, tmNow.tm_hour, tmNow.tm_min);
    auto tb = AddText(a.layout, buf, 0, 12, kHint, kFontXS);
    // Right-align: measure the rendered text and pin to the right edge with
    // 12px padding (AddText has no width; compute via texture size).
    if (tb != nullptr) {
        const s32 w = tb->GetWidth();
        tb->SetX(kScreenWidth - w - 12);
    }
}

TextRef BuildTitle(App& a, const std::string& text, s32 y) {
    return AddText(a.layout, text, kMargin, y, kHint, kFont);
}

void BuildFooter(App& a) {
    // gabagool footer: "A Select   B Back" hints bottom-left, hint color.
    std::string joined;
    for(size_t i = 0; i < a.footerHints.size(); ++i) {
        if(i) joined += "   ";
        joined += a.footerHints[i];
    }
    if(!joined.empty()) {
        AddText(a.layout, joined, kMargin, kFooterY, kHint, kFontXS);
    }
}

MenuRef MakeMenu(s32 x, s32 y, s32 w, s32 itemH, u32 showCount) {
    // gabagool-ish: dark text on white bg, accent focus bar.
    auto menu = pu::ui::elm::Menu::New(x, y, w, kBackground, kAccent, itemH, showCount);
    menu->SetScrollbarColor(kAccent);
    return menu;
}

// ---- ScrollingText (word-wrap + vertical auto-scroll) ----

namespace {

// Greedy word wrap against measured text width; existing newlines are kept.
std::vector<std::string> WrapTextLines(const std::string& text, const std::string& font_name, s32 max_w) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        const std::string para = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        if (para.empty()) {
            out.emplace_back("");
        } else {
            std::string cur;
            size_t wpos = 0;
            while (wpos <= para.size()) {
                size_t sp = para.find(' ', wpos);
                const std::string word = para.substr(wpos, (sp == std::string::npos ? para.size() : sp) - wpos);
                std::string cand = cur.empty() ? word : cur + " " + word;
                s32 cw = 0, ch = 0;
                if (!pu::ui::render::GetTextDimensions(font_name, cand, cw, ch) || cw <= max_w || cur.empty()) {
                    cur = cand;
                } else {
                    out.push_back(cur);
                    cur = word;
                }
                if (sp == std::string::npos) break;
                wpos = sp + 1;
            }
            if (!cur.empty()) out.push_back(cur);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(armGetSystemTick() / 19200);
}

} // namespace

ScrollingText::ScrollingText(s32 x, s32 y, s32 w, s32 h, const std::string& text,
                             const std::string& font_name, pu::ui::Color clr)
    : x_(x), y_(y), w_(w), h_(h), font_name_(font_name), clr_(clr) {
    lines_ = WrapTextLines(text, font_name, w_);
    line_texs_.reserve(lines_.size());
    for (const auto& ln : lines_) {
        line_texs_.push_back(pu::ui::render::RenderText(font_name_, ln.empty() ? " " : ln, clr_));
    }
}

ScrollingText::~ScrollingText() {
    for (auto& t : line_texs_) {
        pu::ui::render::DeleteTexture(t);
    }
}

void ScrollingText::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) {
    if (line_texs_.empty()) return;
    // Total content height and per-line advance.
    s32 line_h = pu::ui::render::GetTextureHeight(line_texs_.front());
    s32 advance = line_h + kLineGap;
    s32 total_h = static_cast<s32>(line_texs_.size()) * advance - kLineGap;

    if (start_ms_ == 0) start_ms_ = NowMs();
    const uint64_t elapsed = NowMs() - start_ms_;
    const s32 max_scroll = total_h > h_ ? (total_h - h_) : 0;
    if (elapsed >= kPauseMs && max_scroll > 0) {
        // ~30 px/s vertical drift after the pause; loop back to top.
        scroll_y_ = static_cast<s32>((elapsed - kPauseMs) / 33) % (max_scroll + 120);
        if (scroll_y_ > max_scroll) scroll_y_ = max_scroll; // hold at end briefly
    }

    // Clip to the viewport, then draw each visible line at its scrolled Y.
    SDL_Renderer* r = pu::ui::render::GetMainRenderer();
    SDL_Rect clip{x, y, w_, h_};
    if (r != nullptr) SDL_RenderSetClipRect(r, &clip);
    s32 ly = y - scroll_y_;
    for (auto& t : line_texs_) {
        if (ly + line_h >= y && ly <= y + h_) {
            drawer->RenderTexture(t, x, ly);
        }
        ly += advance;
    }
    if (r != nullptr) SDL_RenderSetClipRect(r, nullptr);
}

void AddScrollingText(pu::ui::Layout::Ref& layout, const std::string& text,
                      s32 x, s32 y, s32 w, s32 h) {
    layout->Add(ScrollingText::New(x, y, w, h, text, kFontS, kHint));
}

// ---- QrCodeView (nayuki qrcodegen rasterizer) ----

QrCodeView::QrCodeView(s32 x, s32 y, s32 size, const std::string& data) : x_(x), y_(y), size_(size) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    if (!data.empty() && qrcodegen_encodeText(data.c_str(), temp, qrcode,
                                              qrcodegen_Ecc_MEDIUM, 1, 10,
                                              qrcodegen_Mask_AUTO, true)) {
        const int n = qrcodegen_getSize(qrcode);
        // Quiet zone: 4 modules (spec default), then 2px per module scale.
        const int quiet = 4;
        const int cells = n + 2 * quiet;
        const int px = size_ / cells;
        if (px < 1) return;
        const int img = px * cells;
        SDL_Surface* srf = SDL_CreateRGBSurface(0, img, img, 32,
                                                0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        if (srf == nullptr) return;
        SDL_FillRect(srf, nullptr, SDL_MapRGB(srf->format, 0xFF, 0xFF, 0xFF));
        SDL_Rect cell{x, y, 0, 0};
        cell.w = px;
        cell.h = px;
        for (int my = 0; my < n; ++my) {
            for (int mx = 0; mx < n; ++mx) {
                if (qrcodegen_getModule(qrcode, mx, my)) {
                    cell.x = (mx + quiet) * px;
                    cell.y = (my + quiet) * px;
                    SDL_FillRect(srf, &cell, SDL_MapRGB(srf->format, 0x00, 0x00, 0x00));
                }
            }
        }
        // Center the bitmap inside the requested element box.
        x_ += (size_ - img) / 2;
        y_ += (size_ - img) / 2;
        tex_ = pu::ui::render::ConvertToTexture(srf); // frees srf
    }
}

QrCodeView::~QrCodeView() {
    if (tex_ != nullptr) {
        pu::ui::render::DeleteTexture(tex_);
    }
}

void QrCodeView::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) {
    if (tex_ != nullptr) {
        drawer->RenderTexture(tex_, x, y);
    }
}

void AddQrCode(pu::ui::Layout::Ref& layout, const std::string& data, s32 x, s32 y, s32 size) {
    layout->Add(QrCodeView::New(x, y, size, data));
}

void BuildOptionsScreen(App& a, const std::string& title,
                        const std::vector<OptionRow>& rows,
                        std::function<void()> onBack,
                        const std::vector<std::string>& footerHints) {
    a.layout->Clear();
    a.title = title;
    a.footerHints = footerHints.empty() ? std::vector<std::string>{"A Select", "B Back"}
                                        : footerHints;
    BuildStatusBar(a);
    BuildTitle(a, title);
    BuildFooter(a);

    u32 shown = 0;
    for(const auto& r : rows) {
        if(r.visible) ++shown;
    }
    auto menu = MakeMenu(kMenuX, kMenuY, kMenuW, kMenuItemH, std::min<u32>(shown, kMenuVisibleItems));
    menu->SetOnSelectionChanged(nullptr);
    // Restore cursor across rebuilds (A-cycle/BumpUi rebuilds this screen;
    // without this the selection snaps back to row 0).
    if (shown > 0) {
        const int saved = a.state().selectedIndex;
        menu->SetSelectedIndex(static_cast<u32>(std::clamp(saved, 0, static_cast<int>(shown) - 1)));
    }

    s32 idx = -1;
    for(const auto& r : rows) {
        ++idx;
        if(!r.visible) continue;
        std::string name = r.label;
        if(!r.options.empty()) {
            name = r.label + ":  " + r.options[static_cast<size_t>(std::max(0, r.current))];
        }
        auto item = pu::ui::elm::MenuItem::New(name);
        item->SetColor(kHint);
        item->AddOnKey([r, idx, menu, &a]() {
            if(r.onSelect) r.onSelect();
            if(!r.options.empty() && r.onCycle) {
                int next = (r.current + 1) % static_cast<int>(r.options.size());
                r.onCycle(next);
            }
        });
        menu->AddItem(item);
    }
    a.layout->Add(menu);

    // Per-screen input: Left/Right cycle, B back, A already on items.
    a.layout->SetOnInput([rows, onBack, &a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        auto menu = std::static_pointer_cast<pu::ui::elm::Menu>(a.layout->GetElements().back());
        const s32 sel = menu->GetSelectedIndex();
        // Persist the live cursor before any branch (same anti-snap rule as
        // the list screens: rebuilds restore from state().selectedIndex).
        a.state().selectedIndex = sel;
        // Find visible row for sel.
        const OptionRow* row = nullptr;
        s32 vis = 0;
        for(const auto& r : rows) {
            if(!r.visible) continue;
            if(vis == sel) { row = &r; break; }
            ++vis;
        }

        if(row && !row->options.empty() && row->onCycle) {
            if(down & HidNpadButton_Left) {
                int prev = (row->current - 1 + static_cast<int>(row->options.size())) % static_cast<int>(row->options.size());
                row->onCycle(prev);
            } else if(down & HidNpadButton_Right) {
                int next = (row->current + 1) % static_cast<int>(row->options.size());
                row->onCycle(next);
            }
        }
        if(down & HidNpadButton_B) {
            if(onBack) onBack();
        }
    });
}

MenuRef BuildListScreen(App& a, const std::string& title,
                        const std::vector<std::string>& items,
                        std::function<void(int)> onActivate,
                        std::function<void()> onBack,
                        const std::vector<std::string>& footerHints) {
    a.layout->Clear();
    a.title = title;
    a.footerHints = footerHints.empty() ? std::vector<std::string>{"A Select", "B Back"}
                                        : footerHints;
    BuildStatusBar(a);
    BuildTitle(a, title);
    BuildFooter(a);

    auto menu = MakeMenu(kMenuX, kMenuY, kMenuW, kMenuItemH,
                         static_cast<u32>(std::min<size_t>(items.size(), kMenuVisibleItems)));
    for(const auto& it : items) {
        auto item = pu::ui::elm::MenuItem::New(it);
        item->SetColor(kHint);
        menu->AddItem(item);
    }
    a.layout->Add(menu);

    a.layout->SetOnInput([onBack, &a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if(down & HidNpadButton_B) {
            if(onBack) onBack();
        }
    });
    (void)onActivate;
    return menu;
}

} // namespace romm::ui
