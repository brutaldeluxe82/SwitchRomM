// main.cpp — TicromM entry point (Grout-parity Plutonium UI).
// Owns: libnx service init, config/token load, RomServices start, the
// Plutonium Application with the shared Layout, the screen stack, and the
// frame loop (PollJobs + rebuild current screen when data revisions change).
// All view drawing lives in source/ui/*; legacy SDL drawing is gone.

#include <switch.h>
#include <pu/ui/ui_Application.hpp>
#include <pu/ui/ui_Layout.hpp>
#include <pu/ui/render/render_Renderer.hpp>

#include "romm/config.hpp"
#include "romm/logger.hpp"
#include "romm/status.hpp"
#include "romm/version.hpp"
#include "romm/self_update.hpp"
#include "romm/filesystem.hpp"
#include "romm/http_common.hpp"
#include "romm/save_sync.hpp"
#include "romm/platform_prefs.hpp"
#include "romm/downloader.hpp"
#include "romm/queue_store.hpp"

#include "ui/app_shell.hpp"
#include "ui/services.hpp"
#include "ui/screens.hpp"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <functional>
namespace {

// Crash forensics: unhandled C++ exceptions (std::terminate) and fatal
// signals (SIGSEGV etc.) are logged to the SD log with a return-address
// chain before dying, so a crash after "paired" (or anywhere else) leaves
// evidence in sdmc:/switch/TicromM/log.txt instead of vanishing.
void LogCrashFrameChain(const char* what) {
    romm::logLine(std::string("CRASH: ") + what);
    // Walk the frame pointer chain (AAPCS64: fp=x29). Best-effort; stops on
    // the first implausible frame. Offsets pair with the .elf via addr2line.
    struct Frame { Frame* next; void* lr; };
    Frame* fp = nullptr;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    for (int i = 0; fp != nullptr && i < 24; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "  frame %d: lr=%p", i, fp->lr);
        romm::logLine(buf);
        Frame* next = fp->next;
        if (next <= fp) break; // chain must move upward
        fp = next;
    }
    romm::logLine("CRASH END (lr addrs -> 'addr2line -e TicromM.elf')");
}

void CrashTerminateHandler() {
    LogCrashFrameChain("std::terminate (unhandled exception or noexcept exit)");
    romm::shutdownLogFile();
    // Horizon fatal screen (error report + registers) - better than a silent
    // exit; fatalThrow also stops threads cleanly.
    fatalThrowWithPolicy(0xCAFEBABE, FatalPolicy_ErrorReportAndErrorScreen);
}

void CrashSignalHandler(int sig) {
    const char* name = (sig == SIGSEGV) ? "SIGSEGV (null/bad pointer deref)"
                     : (sig == SIGABRT) ? "SIGABRT (abort)"
                     : (sig == SIGBUS)  ? "SIGBUS"
                     : (sig == SIGFPE)  ? "SIGFPE"
                                        : "SIG(unknown)";
    LogCrashFrameChain(name);
    romm::shutdownLogFile();
    fatalThrowWithPolicy(0xDEAD0000u + static_cast<uint32_t>(sig), FatalPolicy_ErrorReportAndErrorScreen);
}

void InstallCrashHandlers() {
    std::set_terminate(CrashTerminateHandler);
    // devkitA64 newlib exposes signal() only (no sigaction). SIGKILL can't
    // be caught; the rest cover the realistic crash modes.
    std::signal(SIGSEGV, CrashSignalHandler);
    std::signal(SIGABRT, CrashSignalHandler);
    std::signal(SIGBUS, CrashSignalHandler);
    std::signal(SIGFPE, CrashSignalHandler);
    std::signal(SIGILL, CrashSignalHandler);
}

} // namespace


constexpr const char* kUpdatePendingPath = "sdmc:/switch/TicromM/update_pending.txt";

// RomApp — pu::ui::Application subclass; OnLoad builds the first screen.
class RomApp final : public pu::ui::Application {
    public:
        RomApp(pu::ui::render::Renderer::Ref renderer)
            : pu::ui::Application(renderer) {}
        void OnLoad() override;
        // Plutonium keeps ovl protected; RomApp exposes read access so the
        // render callback can defer toast overlays to overlay-free frames.
        pu::ui::Overlay::Ref GetOverlayRef() { return this->ovl; }
        romm::ui::App shell;
};

// Frame-time screen rebuild: when any data revision the current screen reads
// changed (uiRevision), rebuild its elements. Cheap when nothing changed.
class ScreenRevisionWatch {
    public:
        explicit ScreenRevisionWatch(romm::ui::RomServices* s) : services(s) {}
        // Returns true when the current screen should be rebuilt.
        bool Consume() {
            const uint64_t rev = services->uiRevision.load();
            if (rev != lastRev) {
                lastRev = rev;
                return true;
            }
            return false;
        }
    private:
        romm::ui::RomServices* services;
        uint64_t lastRev{0};
};


void RomApp::OnLoad() {
    // Wire the shell's non-owning Application handle FIRST: every
    // ShowToast/ShowDialog dereferences it. (It used to be a never-assigned
    // shared_ptr -> null deref on the first toast/dialog.)
    this->shell.app = this;
    if (shell.services->config.serverUrl.empty()) {
        shell.pushScreen(static_cast<int>(romm::ui::ScreenId::ServerAuth));
    } else {
        shell.pushScreen(static_cast<int>(romm::ui::ScreenId::PlatformSelection));
    }
    romm::ui::BuildCurrentScreen(shell);
    this->LoadLayout(shell.layout);
    this->AddRenderCallback([this]() {
        // Render callbacks run before element input each frame, so applying
        // pending navigation here means the rebuilt screen (fresh layout
        // elements and input callbacks) is in place before any button is
        // dispatched — and no rebuild ever runs while an input handler is
        // on the stack (see App header note).
        //
        // Dialog caveat: ShowDialog runs a nested render loop (render-over).
        // Layout input is suppressed there, but render callbacks are NOT —
        // a PollJobs rebuild or navPending apply inside it would free the
        // layout input lambda that is mid-execution on this call stack
        // (the logout crash). So while any dialog is up, freeze everything.
        if (this->shell.dialogActive) {
            return;
        }
        if (this->shell.navPending) {
            this->shell.navPending = false;
            if (!this->shell.stack.empty()) {
                romm::ui::BuildCurrentScreen(this->shell);
            }
        }
        auto* s = this->shell.services;
        s->uiVisibleScreen = this->shell.stack.empty()
                                 ? -1
                                 : this->shell.stack.back().screenId;
        // Pairing success: 3s "Successfully paired!" banner, then unwind to
        // the top-level platform view and refresh the platform index (the
        // token now authorizes it). Consumed here (shell context) because
        // RomServices has no shell handle.
        if (s->pairSuccessPending.exchange(false)) {
            this->shell.queueToast("Successfully paired!");
            this->shell.QueueNavAfter(static_cast<int>(romm::ui::ScreenId::PlatformSelection), 3000);
        }
        if (this->shell.TickDeferredNav()) {
            s->RefetchPlatforms();
        }
        if (!this->GetOverlayRef()) {
            this->shell.DrainPendingToast();
        }
        static ScreenRevisionWatch watch(s);
        s->PollJobs();
        if (s->quitRequested.load()) {
            // Close(false): stop the render loop and unwind through main()'s
            // teardown normally. Close(true) calls exit(0) mid-applet, which
            // under hbmenu/netloader tears down libnx services while still
            // active and hard-crashes the OS.
            this->Close(false);
            return;
        }
        if (watch.Consume()) {
            romm::ui::BuildCurrentScreen(this->shell);
        }
    });
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    socketInitializeDefault();
#if __has_include(<switch/nxlink.h>)
    {
        int nxfd = nxlinkStdio();
        if (nxfd >= 0) {
            romm::logLine("nxlink stdout active.");
        } else {
            consoleDebugInit(debugDevice_SVC);
        }
    }
#else
    consoleDebugInit(debugDevice_SVC);
#endif
    nifmInitialize(NifmServiceType_User);
    fsdevMountSdmc();
    timeInitialize();
    psmInitialize();
    romm::initLogFile();
    // From here on, any terminate/signal lands in the SD log with a frame
    // chain, then Horizon's fatal screen with registers.
    InstallCrashHandlers();
    romm::logLine("Startup (Grout-parity Plutonium UI).");

    // Self-update apply-on-launch (ported unchanged).
    std::string selfNroPath;
    if (argc > 0 && argv && argv[0]) selfNroPath = argv[0];
    selfNroPath = romm::canonicalSelfNroPath(selfNroPath);
    romm::logLine("Self NRO path: " + selfNroPath);
    (void)romm::applyPendingSelfUpdate(selfNroPath, kUpdatePendingPath, [](const std::string& msg) {
        romm::logLine(msg);
    });

    // Services + config.
    auto services = std::make_unique<romm::ui::RomServices>();
    std::string cfgError;
    romm::ErrorInfo cfgErrInfo;
    if (romm::loadConfig(services->config, cfgError, &cfgErrInfo)) {
        romm::setLogLevelFromString(services->config.logLevel);
        romm::logLine("Config loaded: server_url=" + services->config.serverUrl);
        // Prefer the paired device token (device_token.json).
        romm::DeviceToken tok;
        if (romm::loadDeviceToken(romm::kDeviceTokenPath, tok)) {
            services->config.apiToken = tok.accessToken;
            romm::logLine("Using paired device token for API auth.");
        }
        // Platform prefs for the planner.
        {
            std::string prefsErr;
            romm::PlatformPrefs prefs;
            if (romm::loadPlatformPrefs(services->config.platformPrefsMode,
                                        services->config.platformPrefsPathSd,
                                        services->config.platformPrefsPathRomfs,
                                        prefs, prefsErr)) {
                services->status.platformPrefs = prefs;
            } else {
                services->status.platformPrefs = romm::defaultPlatformPrefs();
            }
        }
        romm::ensureDirectory(services->config.downloadDir);
        std::string histErr;
        if (!romm::loadLocalManifests(services->status, services->config, histErr) && !histErr.empty()) {
            romm::logLine("Manifest load warning: " + histErr);
        }
        std::string queueErr;
        if (!romm::loadQueueState(services->status, services->config, queueErr) && !queueErr.empty()) {
            romm::logLine("Queue state load warning: " + queueErr);
        }
        // Fetch platforms before the UI starts (blocking, like the old flow).
        std::string err;
        romm::ErrorInfo errInfo;
        if (!romm::fetchPlatforms(services->config, services->status, err, &errInfo)) {
            std::lock_guard<std::mutex> lock(services->status.mutex);
            services->status.lastError = err;
            services->status.lastErrorInfo = errInfo.code == romm::ErrorCode::None
                ? romm::classifyError(err, romm::ErrorCategory::Network) : errInfo;
            romm::logLine("Failed to fetch platforms: " + err);
        }
        services->ApplyPlatformFilter(); // hide non-tico platforms when enabled
    } else {
        romm::logLine(cfgError);
        std::lock_guard<std::mutex> lock(services->status.mutex);
        services->status.lastError = cfgError;
        services->status.lastErrorInfo = cfgErrInfo.code == romm::ErrorCode::None
            ? romm::classifyError(cfgError, romm::ErrorCategory::Config) : cfgErrInfo;
    }

    // Keep the screen awake while downloads run.

    // UI hooks (wired after the App exists; Start() before them is fine).
    services->Start();

    // Plutonium renderer + application.
    pu::ui::render::RendererInitOptions initOpts(
        SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER,
        pu::ui::render::RendererHardwareFlags);
    initOpts.SetPlServiceType(PlServiceType_User);
    initOpts.AddDefaultSharedFont(PlSharedFontType_Standard);
    initOpts.AddExtraDefaultFontSize(27);
    initOpts.AddExtraDefaultFontSize(30);
    initOpts.AddExtraDefaultFontSize(37);
    initOpts.UseImage(pu::ui::render::ImgAllFlags);
    initOpts.SetInputPlayerCount(1);
    // No1 covers docked controllers; Handheld covers Joy-Cons attached to the
    // console (libnx padInitializeDefault = No1 + Handheld). Without Handheld,
    // padUpdate reads nothing in handheld mode -> no input at all.
    initOpts.AddInputNpadIdType(HidNpadIdType_No1);
    initOpts.AddInputNpadIdType(HidNpadIdType_Handheld);
    auto renderer = pu::ui::render::Renderer::New(initOpts);
    RomApp app(renderer);
    app.shell.services = services.get();
    app.shell.layout = pu::ui::Layout::New();
    app.shell.layout->SetBackgroundColor(romm::ui::kBackground);
    // Navigation is deferred to the render callback via navPending; the hook
    // only marks it. Never rebuild synchronously inside input callbacks.
    app.shell.onNavigate = [&app]() {
        app.shell.navPending = true;
    };

    // Wire nav/toast into the shell. pushScreen/popScreen mark navPending;
    // the render callback rebuilds before the next frame's input dispatch.
    services->navHook = [&app](romm::ui::ScreenId id, romm::ui::NavOp op) {
        switch (op) {
            case romm::ui::NavOp::Push:
                app.shell.pushScreen(static_cast<int>(id));
                break;
            case romm::ui::NavOp::Pop:
                app.shell.popScreen();
                break;
            case romm::ui::NavOp::ReplaceAll:
                // Fetch landing on GameList: keep everything below the top
                // GameList entry (PlatformSelection stays for B-back). If the
                // top is already the same screen, just replace it in place.
                if (!app.shell.stack.empty() &&
                    app.shell.stack.back().screenId == static_cast<int>(id)) {
                    // Already on this screen (e.g. user opened GameList
                    // before the page landed): no-op, just re-render.
                } else {
                    app.shell.pushScreen(static_cast<int>(id));
                }
                break;
        }
    };
    services->toastHook = [&app](const std::string& msg) {
        // Worker threads / PollJobs fire this; never start an overlay from
        // here directly (frame-context hazard). Queue; the render callback
        // drains one per frame.
        app.shell.queueToast(msg);
    };

    if (R_SUCCEEDED(app.Load())) {
        app.ShowWithFadeIn();
    }

    // Teardown.
    romm::logLine("Exiting main loop.");
    services->Stop();
    services->PersistQueueState();
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
    romm::shutdownLogFile();
    romm::httpShutdown();
    psmExit();
    timeExit();
    nifmExit();
    socketExit();
    return 0;
}
