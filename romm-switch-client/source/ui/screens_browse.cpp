// screens_browse.cpp — Platform selection, game list, details, options,
// search, filters, download manager. Grout parity: platform_selection.go,
// games_list.go, game_details.go, game_options.go, search.go, game_filters.go,
// download.go.

#include "screens.hpp"
#include "services.hpp"
#include "romm/api.hpp"

#include <switch.h>
#include "romm/logger.hpp"
#include "romm/version.hpp"
#include "romm/planner.hpp"
#include "romm/queue_policy.hpp"
#include "romm/downloader.hpp"

#include <cstdio>
#include <algorithm>

namespace romm::ui {

namespace {

bool PromptText(const char* header, const char* guide, std::string& inout) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetSubText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, inout.c_str());
    swkbdConfigSetStringLenMax(&kbd, 250);
    char buf[256] = {};
    rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    inout = buf;
    return true;
}

std::string HumanSize(uint64_t bytes) {
    char buf[32];
    if (bytes >= (100ULL << 30)) {
        snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (100ULL << 20)) {
        snprintf(buf, sizeof(buf), "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= (1ULL << 20)) {
        snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= (1ULL << 10)) {
        snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

const char* FilterLabel(RomFilter f) {
    switch (f) {
        case RomFilter::Queued: return "Queued";
        case RomFilter::Resumable: return "Resumable";
        case RomFilter::Failed: return "Failed";
        case RomFilter::Completed: return "Completed";
        case RomFilter::NotQueued: return "Not queued";
        default: return "All";
    }
}

const char* SortLabel(RomSort s) {
    switch (s) {
        case RomSort::TitleDesc: return "Title Z-A";
        case RomSort::SizeDesc: return "Size 9-1";
        case RomSort::SizeAsc: return "Size 1-9";
        default: return "Title A-Z";
    }
}

} // namespace

// ---- Platform Selection (platform_selection.go) ----

void BuildPlatformSelectionScreen(App& a) {
    auto* s = a.services;
    std::vector<Platform> plats;
    std::string netBusyWhat;
    bool netBusy = false;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        plats = s->status.platforms;
        netBusy = s->status.netBusy.load();
        netBusyWhat = s->status.netBusyWhat;
    }

    a.layout->Clear();
    a.title = "Grout";
    // Grout footer order (platform_selection.go): X Settings, Y Sync,
    // A Select, B Quit. Minus = one-button Download Manager access across
    // all library views (user ask; replaces the inconsistent Y-toast flow).
    a.footerHints = {"A Select", "Y Queue All", "X Settings", "- Queue", "+ Exit"};
    BuildStatusBar(a);
    BuildTitle(a, "Grout");
    BuildFooter(a);

    if (netBusy) {
        AddText(a.layout, netBusyWhat.empty() ? "Loading..." : netBusyWhat, kMargin, 210, kAccent, kFontM);
    } else if (plats.empty()) {
        AddText(a.layout, "No platforms found. Check Settings > Server Address.", kMargin, 210, kHint, kFontM, 1740);
    }

    std::vector<std::string> items;
    items.reserve(plats.size());
    for (const auto& p : plats) {
        items.push_back(p.name + " (" + std::to_string(p.romCount) + ")");
    }
    if (!items.empty()) {
        auto menu = MakeMenu(kMenuX, kMenuY, kMenuW, kMenuItemH,
                             static_cast<u32>(std::min<size_t>(items.size(), kMenuVisibleItems)));
        // Restore cursor from retained state.
        for (const auto& it : items) {
            auto mi = pu::ui::elm::MenuItem::New(it);
            mi->SetColor(kHint);
            menu->AddItem(mi);
        }
        a.layout->Add(menu);
        a.layout->SetOnInput([s, &a, plats](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
            // The menu element consumes Up/Down and A (via OnKey) itself; we
            // handle the remaining shell buttons here.
            auto menu = std::static_pointer_cast<pu::ui::elm::Menu>(a.layout->GetElements().back());
            const s32 sel = menu->GetSelectedIndex();
            if (down & HidNpadButton_A) {
                if (sel >= 0 && sel < static_cast<s32>(plats.size())) {
                    const Platform& p = plats[static_cast<size_t>(sel)];
                    // Fetch-apply owns the GameList push (navHook ReplaceAll
                    // after the page lands) — pushing here too would double-
                    // enter the list and corrupt B-back. The platform screen
                    // shows the busy line meanwhile.
                    a.state().selectedIndex = sel;
                    s->SubmitRomFetch(p.id, p.slug, p.name, true);
                }
            } else if (down & HidNpadButton_Y) {
                // Grout's Y on the platform list = Sync; here it queues the
                // highlighted platform's entire ROM library (fetch pages +
                // bulk-enqueue). Toast mirrors the ROM-list bulk feedback.
                if (sel >= 0 && sel < static_cast<s32>(plats.size())) {
                    const Platform& p = plats[static_cast<size_t>(sel)];
                    s->QueuePlatformBulk(p);
                    a.queueToast("Queuing all ROMs from " + p.name);
                }
            } else if (down & HidNpadButton_Minus) {
                // One-button Download Manager access.
                a.pushScreen(static_cast<int>(ScreenId::DownloadManager));
            } else if (down & HidNpadButton_X) {
                a.pushScreen(static_cast<int>(ScreenId::Settings));
            } else if (down & HidNpadButton_Plus) {
                // Top-level view: + exits. B does nothing here (accidental-
                // exit guard); B remains "back" on every pushed view.
                s->quitRequested.store(true);
            }
        });
    } else {
        a.layout->SetOnInput([&a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
            if (down & HidNpadButton_X) a.pushScreen(static_cast<int>(ScreenId::Settings));
        });
    }
}

// ---- Game List (games_list.go) ----

void BuildGameListScreen(App& a) {
    auto* s = a.services;
    std::vector<Game> roms;
    std::string platName;
    std::string searchQ;
    RomFilter filter;
    RomSort sort;
    bool netBusy = false;
    std::string netBusyWhat;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        roms = s->status.roms;
        platName = s->status.currentPlatformName;
        searchQ = s->status.romSearchQuery;
        filter = s->status.romFilter;
        sort = s->status.romSort;
        netBusy = s->status.netBusy.load();
        netBusyWhat = s->status.netBusyWhat;
    }

    // Header state, gabagool-style: [Filtered (Queued)] prefixes so the
    // active filter is visible. Sort only shows when non-default (A-Z is
    // the implicit baseline; showing it would be noise).
    std::string title = platName.empty() ? "Games" : platName;
    if (!searchQ.empty()) title = "[Search: \"" + searchQ + "\"] " + title;
    if (filter != RomFilter::All) title = "[Filtered (" + std::string(FilterLabel(filter)) + ")] " + title;
    if (sort != RomSort::TitleAsc) title = "[Sort: " + std::string(SortLabel(sort)) + "] " + title;

    a.layout->Clear();
    a.title = title;
    const bool msActive = a.state().multiSelectActive;
    a.footerHints = msActive
        ? std::vector<std::string>{"A Queue Selected", "Y Toggle", "- Queue", "B Exit Select"}
        : std::vector<std::string>{"B Back", "A Select", "Y Multi-Select", "X Search", "L Filter", "R Sort", "- Queue"};
    BuildStatusBar(a);
    BuildTitle(a, title, 36);
    BuildFooter(a);

    if (netBusy) {
        AddText(a.layout, netBusyWhat.empty() ? "Loading..." : netBusyWhat, kMargin, 180, kAccent, kFontM);
    } else if (roms.empty()) {
        AddText(a.layout, "No games found for this platform.", kMargin, 195, kHint, kFontM);
    }

    auto menu = MakeMenu(kMenuX, kMenuY, kMenuW, kMenuItemH, kMenuVisibleItems);
    for (const auto& g : roms) {
        std::string label = g.title;
        if (g.sizeBytes > 0) label += "   " + HumanSize(g.sizeBytes);
        // Grout-style multi-select marker on selected rows.
        if (msActive && a.state().selectedIds.count(g.id)) label = "[x] " + label;
        auto mi = pu::ui::elm::MenuItem::New(label);
        mi->SetColor(kHint);
        menu->AddItem(mi);
    }
    a.layout->Add(menu);

    // Restore cursor.
    if (!roms.empty()) {
        int sel = 0;
        {
            std::lock_guard<std::mutex> lock(s->status.mutex);
            sel = s->status.selectedRomIndex;
        }
        menu->SetSelectedIndex(static_cast<u32>(std::max(0, std::min<int>(sel, static_cast<int>(roms.size()) - 1))));
    }
    // Grout games_list.go footer: B Back, Y Filters, X Search. Here Y enters
    // multi-select (queueing), X Search, L Filter, R Sort, and Minus is the
    // one-button Download Manager access on every library view.
    a.footerHints = msActive
        ? std::vector<std::string>{"A Queue Selected", "Y Toggle", "B Exit Select"}
        : std::vector<std::string>{"B Back", "A Select", "Y Multi-Select", "X Search",
                                   "L Filter", "R Sort", "- Queue"};
    a.layout->Clear();
    BuildStatusBar(a);
    BuildTitle(a, title, 36);
    BuildFooter(a);
    a.layout->Add(menu);

    a.layout->SetOnInput([s, &a, roms](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        auto menu = std::static_pointer_cast<pu::ui::elm::Menu>(a.layout->GetElements().back());
        const s32 sel = menu->GetSelectedIndex();
        // Track the live cursor before any branch: rebuilds restore from
        // selectedRomIndex, so an unstored cursor snaps the list back to the
        // top (the multi-select "cursor bump" bug).
        {
            std::lock_guard<std::mutex> lock(s->status.mutex);
            s->status.selectedRomIndex = sel;
        }
        auto& st = a.state();
        if (down & HidNpadButton_A) {
            if (st.multiSelectActive) {
                // A confirms: queue every selected ROM (bulk), then open the
                // manager so the queue is visible/controllable immediately.
                std::vector<Game> chosen;
                for (const auto& g : roms) {
                    if (st.selectedIds.count(g.id)) chosen.push_back(g);
                }
                st.multiSelectActive = false;
                st.selectedIds.clear();
                if (!chosen.empty() && s->EnqueueGamesBulk(chosen) > 0) {
                    a.queueToast("Queued " + std::to_string(chosen.size()) + " ROM(s)");
                    a.pushScreen(static_cast<int>(ScreenId::DownloadManager));
                } else {
                    a.RebuildCurrent();
                }
            } else {
                a.pushScreen(static_cast<int>(ScreenId::GameDetails));
            }
        } else if (down & HidNpadButton_Y) {
            // Y = multi-select entry/toggle (Grout's select-button role);
            // never opens the manager — Minus does that on every view.
            if (st.multiSelectActive) {
                // Toggle the highlighted row's selection.
                if (sel >= 0 && sel < static_cast<int>(roms.size())) {
                    const std::string& id = roms[static_cast<size_t>(sel)].id;
                    if (!st.selectedIds.erase(id)) st.selectedIds.insert(id);
                }
                a.RebuildCurrent(); // re-render with [x] markers
            } else {
                st.multiSelectActive = true;
                st.selectedIds.clear();
                a.RebuildCurrent();
            }
        } else if (down & HidNpadButton_Minus) {
            // One-button Download Manager access (consistent across views).
            a.pushScreen(static_cast<int>(ScreenId::DownloadManager));
        } else if (st.multiSelectActive && (down & HidNpadButton_B)) {
            st.multiSelectActive = false;
            st.selectedIds.clear();
            a.RebuildCurrent();
        } else if (down & HidNpadButton_L) {
            s->CycleRomFilter(-1);
        } else if (down & HidNpadButton_R) {
            s->CycleRomSort(1);
        } else if (down & HidNpadButton_X) {
            std::string q;
            if (PromptText("Search", "Search this platform", q)) {
                s->SetRomSearchQuery(q);
            }
        } else if (down & HidNpadButton_B) {
            a.popScreen();
            s->SetRomSearchQuery(""); // Grout: leaving the list clears search
        }
    });
}

// ---- Game Details (game_details.go) ----

void BuildGameDetailsScreen(App& a) {
    auto* s = a.services;
    Game g;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        int sel = s->status.selectedRomIndex;
        if (sel >= 0 && sel < static_cast<int>(s->status.roms.size())) {
            g = s->status.roms[static_cast<size_t>(sel)];
        }
    }

    // List rows carry no files[]; enrich once so Files/Size/description are
    // real. Enriched row replaces the list entry, so the fix persists.
    if (!g.id.empty() && g.files.empty()) {
        Game enriched = g;
        std::string err;
        if (romm::enrichGameWithFiles(s->config, enriched, err, nullptr)) {
            std::lock_guard<std::mutex> lock(s->status.mutex);
            for (auto& mg : s->status.romsAll) {
                if (mg.id == enriched.id) { mg = enriched; break; }
            }
            for (auto& mg : s->status.roms) {
                if (mg.id == enriched.id) { mg = enriched; break; }
            }
            s->RebuildVisibleRomsLocked(false);
            g = enriched;
        }
    }

    a.layout->Clear();
    a.title = g.title;
    a.footerHints = {"A Download", "Y Options", "- Queue", "B Back"};
    BuildStatusBar(a);
    BuildTitle(a, g.title, 36);
    BuildFooter(a);

    // Columns (1080p grid): boxart 420px | details 480px | description fills
    // the rest to the right margin (Grout's game details).
    const s32 boxX = kMargin;
    const s32 boxW = 420;
    const s32 detX = boxX + boxW + 60;
    const s32 detW = 480;
    const s32 descX = detX + detW + 60;
    const s32 descW = kScreenWidth - kMargin - descX;

    s32 y = 150;
    if (!g.coverUrl.empty()) {
        s->RequestCover(g.coverUrl, g.title);
    }
    if (s->currentCoverKey == g.coverUrl && !s->currentCover.pixels.empty()) {
        // RGBA pixels -> SDL surface -> texture -> Image element. Cover
        // completion bumps uiRevision, so this draws on first entry too.
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
            const_cast<unsigned char*>(s->currentCover.pixels.data()),
            s->currentCover.w, s->currentCover.h, 32, s->currentCover.w * 4,
            SDL_PIXELFORMAT_ABGR8888);
        if (surf) {
            auto tex = pu::ui::render::ConvertToTexture(surf);
            if (tex) {
                auto handle = pu::sdl2::TextureHandle::New(tex);
                auto img = pu::ui::elm::Image::New(boxX, y, handle);
                img->SetWidth(boxW);
                img->SetHeight(560);
                a.layout->Add(img);
            }
        }
    }
    if (g.sizeBytes > 0) {
        AddText(a.layout, "File Size: " + HumanSize(g.sizeBytes), detX, y, kHint, kFontS);
    }
    if (!g.platformSlug.empty()) {
        AddText(a.layout, "Platform: " + g.platformSlug, detX, y + 60, kHint, kFontS);
    }
    AddText(a.layout, "Files: " + std::to_string(g.files.size()), detX, y + 120, kHint, kFontS);

    // Description renders via ScrollingText (word-wrap + vertical scroll with
    // a 3s pause before scrolling starts) in the viewport column.
    if (!g.description.empty()) {
        AddScrollingText(a.layout, g.description, descX, y, descW, kDescViewportH);
    } else {
        AddText(a.layout, "(no description)", descX, y, kHint, kFontS, descW);
    }

    a.layout->SetOnInput([s, &a, g](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) {
            a.popScreen();
        } else if (down & HidNpadButton_A) {
            if (!g.id.empty() && s->EnqueueGame(g)) {
                a.queueToast("Download queued: " + g.title);
                a.pushScreen(static_cast<int>(ScreenId::DownloadManager));
            }
        } else if (down & HidNpadButton_Minus) {
            // One-button Download Manager access (consistent across views).
            a.pushScreen(static_cast<int>(ScreenId::DownloadManager));
        } else if (down & HidNpadButton_Y) {
            a.pushScreen(static_cast<int>(ScreenId::GameOptions));
        }
    });
}

// ---- Game Options (game_options.go) ----

void BuildGameOptionsScreen(App& a) {
    // Game QR/URL share view removed (RomM's per-game share codes aren't a
    // Switch concern; QR is reserved for device pairing).
    BuildOptionsScreen(a, "Game Options",
        {
            {"Save Slot", {"Default"}, 0, nullptr, nullptr},
        },
        [&a]() { a.popScreen(); },
        {"B Back", "A Select"});
}

// ---- Search (search.go) — handled inline via swkbd in list screens ----

// ---- Game Filters (game_filters.go) ----

void BuildGameFiltersScreen(App& a) {
    auto* s = a.services;
    RomFilter filter;
    RomSort sort;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        filter = s->status.romFilter;
        sort = s->status.romSort;
    }
    const std::vector<std::string> filterNames = {"All", "Queued", "Resumable", "Failed", "Completed", "Not queued"};
    const std::vector<std::string> sortNames = {"Title A-Z", "Title Z-A", "Size 9-1", "Size 1-9"};
    int fIdx = static_cast<int>(filter);
    int sIdx = static_cast<int>(sort);

    BuildOptionsScreen(a, "Filters",
        {
            {"Status", filterNames, fIdx, nullptr,
             [s](int idx) {
                 std::lock_guard<std::mutex> lock(s->status.mutex);
                 s->status.romFilter = static_cast<RomFilter>(idx);
                 s->status.romListOptionsRevision++;
                 s->BumpUi();
             }},
            {"Sort", sortNames, sIdx, nullptr,
             [s](int idx) {
                 std::lock_guard<std::mutex> lock(s->status.mutex);
                 s->status.romSort = static_cast<RomSort>(idx);
                 s->status.romListOptionsRevision++;
                 s->BumpUi();
             }},
        },
        [&a]() { a.popScreen(); },
        {"B Cancel", "A Select", "Start Save"});
}

// ---- Download Manager (download.go) ----

void BuildDownloadManagerScreen(App& a) {
    auto* s = a.services;
    auto& st = a.state();
    a.layout->Clear();
    a.title = "Downloads";
    // Grout-style select semantics: Y enters/toggles multi-select delete,
    // A deletes selected rows (or starts the worker in normal mode), X purges
    // the highlighted pending item, Start clears the whole queue.
    a.footerHints = st.multiSelectActive
        ? std::vector<std::string>{"A Delete Selected", "Y Toggle", "B Exit Select"}
        : std::vector<std::string>{"A Start", "X Delete", "L Cancel", "R Retry",
                                   "Y Select", "+ Clear All", "B Back"};
    BuildStatusBar(a);
    BuildTitle(a, "Download Manager", 36);
    BuildFooter(a);

    // The manager reserves a fixed progress strip below the list: the menu
    // shows one item fewer than full-screen so the strip (speed/percent +
    // failed line + separator) owns its own band and never overlaps the
    // footer at kFooterY.
    constexpr u32 kMgrVisibleItems = kMenuVisibleItems - 1; // 5
    constexpr s32 kProgressY = kMenuY + static_cast<s32>(kMgrVisibleItems) * kMenuItemH + 18;
    auto menu = MakeMenu(kMenuX, kMenuY, kMenuW, kMenuItemH, kMgrVisibleItems);
    std::vector<QueueItem> items;
    bool workerRunning = false;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        items = s->status.downloadQueue;
        workerRunning = s->status.downloadWorkerRunning.load();
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& q = items[i];
        const char* state = "Pending";
        switch (q.state) {
            case QueueState::Downloading: state = "Downloading"; break;
            case QueueState::Finalizing: state = "Finalizing"; break;
            case QueueState::Completed: state = "Done"; break;
            case QueueState::Resumable: state = "Paused"; break;
            case QueueState::Failed: state = "Failed"; break;
            case QueueState::Cancelled: state = "Cancelled"; break;
            default: break;
        }
        std::string label = std::string(state) + ": " + q.game.title;
        if (st.multiSelectActive && st.selectedIds.count(q.game.id)) label = "[x] " + label;
        auto mi = pu::ui::elm::MenuItem::New(label);
        mi->SetColor(q.state == QueueState::Failed ? kAccent : kHint);
        menu->AddItem(mi);
    }
    a.layout->Add(menu);
    // Restore cursor clamped to the (possibly shrunk) queue; also persist the
    // clamp so handler-time `sel` can never exceed the row count.
    if (!items.empty()) {
        st.managerCursor = std::clamp(st.managerCursor, 0, static_cast<int>(items.size()) - 1);
        menu->SetSelectedIndex(static_cast<u32>(st.managerCursor));
    } else {
        st.managerCursor = 0;
    }

    // Progress strip: separator bar, live speed/percent, failed line. All
    // inside [kProgressY, kFooterY - 12] so the footer stays clear.
    a.layout->Add(pu::ui::elm::Rectangle::New(kMargin, kProgressY - 8, kMenuW, 2, kHint));
    if (workerRunning && !items.empty()) {
        uint64_t cur = s->status.currentDownloadedBytes.load();
        uint64_t tot = s->status.currentDownloadSize.load();
        const double pct = tot > 0 ? (100.0 * static_cast<double>(cur) / static_cast<double>(tot)) : 0.0;
        char buf[160];
        snprintf(buf, sizeof(buf), "%s: %s / %s  (%.0f%%)  %.1f MB/s",
                 s->status.currentDownloadTitle.c_str(),
                 HumanSize(cur).c_str(), HumanSize(tot).c_str(), pct,
                 s->status.lastSpeedMBps);
        AddText(a.layout, buf, kMargin, kProgressY + 8, kAccent, kFontS, 1740);
        auto bar = pu::ui::elm::ProgressBar::New(kMargin, kProgressY + 64, kMenuW, 8, 100.0);
        bar->SetProgress(pct);
        bar->SetProgressColor(kAccent);
        bar->SetBackgroundColor({0xE0, 0xE0, 0xE0, 0xFF});
        a.layout->Add(bar);
    } else if (!items.empty()) {
        // Idle summary: what a Start press would kick off.
        uint64_t pending = 0;
        size_t n = 0;
        for (const auto& q : items) {
            if (q.state == QueueState::Pending || q.state == QueueState::Failed ||
                q.state == QueueState::Cancelled || q.state == QueueState::Resumable) {
                ++n;
                uint64_t sz = q.bundle.totalSize();
                if (sz == 0) sz = q.game.sizeBytes;
                pending += sz;
            }
        }
        if (n > 0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%zu item(s) ready - %s. Press A to start.",
                     n, HumanSize(pending).c_str());
            AddText(a.layout, buf, kMargin, kProgressY + 8, kHint, kFontS, 1740);
        } else {
            AddText(a.layout, "All items completed.", kMargin, kProgressY + 8, kHint, kFontS, 1740);
        }
    }
    if (s->status.lastDownloadFailed.load() && !s->status.lastDownloadError.empty()) {
        AddText(a.layout, "Failed: " + s->status.lastDownloadError, kMargin, kProgressY + 96, kAccent, kFontS, 1740);
    }
    if (items.empty()) {
        AddText(a.layout, "Download queue is empty.", kMargin, kMenuY, kHint, kFontM);
    }

    a.layout->SetOnInput([s, &a, menu](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        // Capture the menu shared_ptr directly: the menu is not the last
        // layout element here (status/speed texts are added after it), and
        // index-based lookup would either miss or hit a Text element.
        const s32 sel = menu->GetSelectedIndex();
        auto& st = a.state();
        if (st.managerCursor != sel) st.managerCursor = sel;
        if (down & HidNpadButton_B) {
            if (st.multiSelectActive) {
                st.multiSelectActive = false;
                st.selectedIds.clear();
                a.RebuildCurrent();
            } else {
                a.popScreen();
            }
        } else if (down & HidNpadButton_Y) {
            // Grout multi-select: enter mode, then toggle the highlighted
            // row's selection. Row identity = queue order index.
            if (!st.multiSelectActive) {
                st.multiSelectActive = true;
                st.selectedIds.clear();
            } else {
                std::string rowId;
                {
                    std::lock_guard<std::mutex> lock(s->status.mutex);
                    if (sel >= 0 && sel < static_cast<s32>(s->status.downloadQueue.size())) {
                        rowId = s->status.downloadQueue[static_cast<size_t>(sel)].game.id;
                    }
                }
                if (!rowId.empty()) {
                    if (!st.selectedIds.erase(rowId)) st.selectedIds.insert(rowId);
                }
            }
            a.RebuildCurrent();
        } else if (down & HidNpadButton_A) {
            if (st.multiSelectActive) {
                // Delete every selected row.
                bool didDelete = false;
                {
                    std::lock_guard<std::mutex> lock(s->status.mutex);
                    auto& q = s->status.downloadQueue;
                    for (size_t i = 0; i < q.size();) {
                        if (st.selectedIds.count(q[i].game.id) &&
                            q[i].state != QueueState::Downloading &&
                            q[i].state != QueueState::Finalizing) {
                            q.erase(q.begin() + static_cast<long>(i));
                            didDelete = true;
                        } else {
                            ++i;
                        }
                    }
                    if (didDelete) s->status.downloadQueueRevision++;
                }
                st.multiSelectActive = false;
                st.selectedIds.clear();
                if (didDelete) {
                    s->RecomputeTotals();
                    s->PersistQueueState();
                    a.queueToast("Deleted selected");
                }
                a.RebuildCurrent();
            } else {
                // Start downloads (ported StartDownload in QUEUE).
                bool allowStart = false;
                {
                    std::lock_guard<std::mutex> lock(s->status.mutex);
                    allowStart = !s->status.downloadQueue.empty() && !s->status.downloadWorkerRunning.load();
                    if (allowStart) {
                        s->status.currentDownloadIndex.store(0);
                        s->status.currentDownloadedBytes.store(0);
                        s->status.totalDownloadedBytes.store(0);
                        s->status.totalDownloadBytes.store(0);
                        s->status.downloadCompleted = false;
                        for (auto& q : s->status.downloadQueue) {
                            uint64_t sz = q.bundle.totalSize();
                            if (sz == 0) sz = q.game.sizeBytes;
                            s->status.totalDownloadBytes.fetch_add(sz);
                        }
                        if (!s->status.downloadQueue.empty()) {
                            const auto& first = s->status.downloadQueue[0];
                            uint64_t firstSize = first.bundle.totalSize();
                            if (firstSize == 0) firstSize = first.game.sizeBytes;
                            s->status.currentDownloadSize.store(firstSize);
                            s->status.currentDownloadTitle = first.bundle.title.empty()
                                ? first.game.title : first.bundle.title;
                            s->status.downloadQueue[0].state = QueueState::Downloading;
                            s->status.downloadQueueRevision++;
                        }
                    }
                }
                if (allowStart) {
                    s->RecomputeTotals();
                    startDownloadWorker(s->status, s->config);
                    a.queueToast("Downloads started");
                }
            }
        } else if (down & HidNpadButton_X) {
            // Purge the highlighted pending item (mistake removal).
            bool didDelete = false;
            {
                std::lock_guard<std::mutex> lock(s->status.mutex);
                auto& q = s->status.downloadQueue;
                if (sel >= 0 && sel < static_cast<s32>(q.size())) {
                    const auto& item = q[static_cast<size_t>(sel)];
                    if (item.state != QueueState::Downloading &&
                        item.state != QueueState::Finalizing) {
                        // While the worker runs, front (index 0) is the
                        // active item in Downloading/Finalizing state — the
                        // state check above already rejects it. Any other
                        // index is a waiting item and safe to drop.
                        q.erase(q.begin() + sel);
                        didDelete = true;
                        s->status.downloadQueueRevision++;
                    }
                }
            }
            if (didDelete) {
                s->RecomputeTotals();
                s->PersistQueueState();
                a.queueToast("Removed from queue");
            } else {
                a.queueToast("Can't delete the active download (use L to cancel)");
            }
            a.RebuildCurrent();
        } else if (down & HidNpadButton_Plus) {
            // Clear all: stop a running worker, wipe the entire queue.
            bool hadQueue = false;
            {
                std::lock_guard<std::mutex> lock(s->status.mutex);
                hadQueue = !s->status.downloadQueue.empty();
                s->status.downloadQueue.clear();
                s->status.downloadQueueRevision++;
                s->status.downloadCompleted = false;
            }
            if (s->status.downloadWorkerRunning.load()) stopDownloadWorker();
            if (hadQueue) {
                s->RecomputeTotals();
                s->PersistQueueState();
                a.queueToast("Queue cleared");
            } else {
                a.queueToast("Queue already empty");
            }
            a.RebuildCurrent();
        } else if (down & HidNpadButton_L) {
            // Cancel the active download: stop the worker, mark the item
            // resumable (parts preserved) so A restarts it later.
            if (s->status.downloadWorkerRunning.load()) {
                stopDownloadWorker();
                {
                    std::lock_guard<std::mutex> lock(s->status.mutex);
                    if (!s->status.downloadQueue.empty()) {
                        s->status.downloadQueue.front().state = QueueState::Resumable;
                        s->status.downloadQueue.front().error = "Cancelled";
                        s->status.downloadQueueRevision++;
                    }
                    s->status.downloadCompleted = false;
                }
                s->RecomputeTotals();
                s->PersistQueueState();
                a.queueToast("Download cancelled");
            } else {
                a.queueToast("Nothing active to cancel");
            }
        } else if (down & HidNpadButton_R) {
            // Retry failed: move failed items back to pending (worker picks
            // them up on next A; completed files short-circuit via size check).
            bool didRetry = false;
            {
                std::lock_guard<std::mutex> lock(s->status.mutex);
                for (auto& q : s->status.downloadQueue) {
                    if (q.state == QueueState::Failed || q.state == QueueState::Cancelled) {
                        q.state = QueueState::Pending;
                        q.error.clear();
                        didRetry = true;
                    }
                }
                if (didRetry) s->status.downloadQueueRevision++;
            }
            if (didRetry) {
                s->RecomputeTotals();
                s->PersistQueueState();
                a.queueToast("Failed items requeued");
            } else {
                a.queueToast("No failed items");
            }
        }
    });
}

// ---- Game QR (game_qr.go; N/A on Switch, Toast placeholder used in options) ----

} // namespace romm::ui
