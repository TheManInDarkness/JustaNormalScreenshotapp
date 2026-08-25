#include "AppPaths.h"
#include "CaptureController.h"
#include "Clipboard.h"
#include "Common.h"
#include "CrashHandler.h"
#include "DpiHelper.h"
#include "GalleryWindow.h"
#include "HotkeyManager.h"
#include "IconCache.h"
#include "Logger.h"
#include "PDFConvertTool.h"
#include "ScrollableCanvas.h"
#include "SettingsDialog.h"
#include "Settings.h"
#include "StitchTool.h"
#include "Theme.h"
#include "Toast.h"
#include "TrayIcon.h"
#include "Utils.h"
#include "resource.h"

namespace {

constexpr wchar_t kWindowClass[] = L"ScreenshotApp_MessageWindow";
constexpr wchar_t kMutexName[] = L"Local\\ScreenshotApp_SingleInstance";
constexpr wchar_t kWakeMessage[] = L"ScreenshotApp_ShowExistingInstance";

// A hidden window owns the tray icon and the global hotkeys. It exists
// separately from the main window so those keep working while the main
// window is closed to the notification area.
HWND g_hubWindow = nullptr;
HANDLE g_singleInstanceMutex = nullptr;
UINT g_wakeMessage = 0;

void OnCommand(HWND hwnd, int id) {
    switch (id) {
        case ID_TRAY_OPEN:
            Gallery::Show();
            return;

        case ID_TRAY_CAPTURE:
            CaptureController::DoCapture();
            return;

        case ID_TRAY_STITCH:
            StitchTool::Show(Gallery::GetWindow(), {});
            return;

        case ID_TRAY_PDF:
            PDFConvertTool::Show(Gallery::GetWindow(), {});
            return;

        case ID_TRAY_SETTINGS:
            SettingsDialog::Show(Gallery::GetWindow());
            Gallery::Refresh();
            return;

        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            return;

        default:
            return;
    }
}

void OnHotkey(HWND hwnd, int id) {
    Hotkeys::Action action;
    if (!Hotkeys::ActionFromId(id, &action)) return;

    switch (action) {
        case Hotkeys::Action::Capture:
        case Hotkeys::Action::CaptureAlt:
            CaptureController::DoCapture();
            return;
    }
}

LRESULT CALLBACK HubWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == Tray::TaskbarCreatedMessage()) {
        Tray::Readd();
        return 0;
    }
    if (g_wakeMessage && msg == g_wakeMessage) {
        // A second copy was launched; that is a request to see the window.
        Gallery::Show();
        return 0;
    }

    switch (msg) {
        case WM_COMMAND:
            OnCommand(hwnd, LOWORD(wParam));
            return 0;

        case WM_HOTKEY:
            OnHotkey(hwnd, static_cast<int>(wParam));
            return 0;

        case Tray::WM_TRAYICON: {
            // NOTIFYICON_VERSION_4 packs the event into the low word of lParam.
            const UINT event = LOWORD(lParam);
            if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
                // Both, deliberately: a double-click delivers a single click
                // first, so if the two did different things the first half of
                // every double-click would fire something else.
                Gallery::Show();
            } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                Tray::ShowContextMenu(hwnd);
            }
            return 0;
        }

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            if (Theme::IsThemeChangeMessage(msg, lParam)) {
                // Picked up live, without a restart.
                Theme::Refresh();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// True when this is the only instance. Otherwise the already-running one is
// asked to surface its window, and this process should exit.
bool ClaimSingleInstance() {
    g_wakeMessage = RegisterWindowMessageW(kWakeMessage);

    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_singleInstanceMutex) return true;  // cannot tell; carry on

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_wakeMessage) {
            PostMessageW(HWND_BROADCAST, g_wakeMessage, 0, 0);
        }
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        return false;
    }
    return true;
}

bool CreateHubWindow(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HubWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!RegisterClassExW(&wc)) return false;

    // A real (if never shown) top-level window rather than HWND_MESSAGE: a
    // message-only window does not receive the shell's TaskbarCreated
    // broadcast, so the tray icon would never come back after an Explorer
    // restart.
    g_hubWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"ScreenshotApp",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                                  nullptr);
    return g_hubWindow != nullptr;
}

void ReportHotkeyState() {
    const std::wstring failures = Hotkeys::FailureReport();
    if (failures.empty()) return;

    if (Hotkeys::AnyCaptureBindingActive()) {
        // One of the two bindings registered, which is the case the second
        // binding exists for - say so quietly rather than raising an alarm.
        Logger::Info(L"Falling back to the alternate capture hotkey");
        return;
    }

    Toast::Show(L"No capture hotkey is available",
                L"Another app owns them. Open Settings to pick a different one.",
                []() { SettingsDialog::Show(Gallery::GetWindow()); });
}

bool WantsTrayStart(PWSTR commandLine) {
    if (!commandLine) return false;
    std::wstring args = commandLine;
    return args.find(L"--tray") != std::wstring::npos ||
           args.find(L"/tray") != std::wstring::npos;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    if (!ClaimSingleInstance()) return 0;

    Logger::Init();
    CrashHandler::Install();

    // OLE (not just COM) because the clipboard uses OleSetClipboard, and the
    // file dialogs are COM objects.
    const HRESULT oleHr = OleInitialize(nullptr);
    if (FAILED(oleHr)) Logger::Errorf(L"OleInitialize failed (0x%08X)", oleHr);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES |
                ICC_HOTKEY_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES |
                ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&icc);

    if (!Utils::InitGdiplus()) {
        MessageBoxW(nullptr, L"GDI+ could not be initialised. ScreenshotApp cannot run.",
                    L"ScreenshotApp", MB_OK | MB_ICONERROR);
        return 1;
    }

    Settings::Load();
    Settings::ApplyStartupRegistration(Settings::Get());
    Theme::Refresh();

    ScrollableCanvas::RegisterCanvasClass();

    if (!CreateHubWindow(instance)) {
        MessageBoxW(nullptr, L"The application window could not be created.",
                    L"ScreenshotApp", MB_OK | MB_ICONERROR);
        Utils::ShutdownGdiplus();
        return 1;
    }

    if (!Tray::Add(g_hubWindow)) {
        // Not fatal any more: there is a real window to fall back on.
        Logger::Warn(L"The tray icon could not be added");
    }

    Gallery::Create(instance);

    Hotkeys::RegisterAll(g_hubWindow);
    ReportHotkeyState();

    // Launched by the Run key, or explicitly asked to stay out of the way.
    const bool trayStart = WantsTrayStart(commandLine) ||
                           Settings::Get().startMinimizedToTray;
    if (!trayStart) Gallery::Show();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // The main window and the tool dialogs all rely on the loop offering
        // them the message first, or tab/arrow/Esc navigation does nothing.
        HWND gallery = Gallery::GetWindow();
        HWND stitch = StitchTool::GetOpenWindow();
        HWND pdf = PDFConvertTool::GetOpenWindow();
        if (stitch && IsDialogMessageW(stitch, &msg)) continue;
        if (pdf && IsDialogMessageW(pdf, &msg)) continue;
        if (gallery && IsWindowVisible(gallery) && IsDialogMessageW(gallery, &msg)) {
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Hotkeys::UnregisterAll();
    Gallery::Destroy();
    Tray::Remove();
    Toast::Shutdown();
    // No Clipboard::Shutdown: the copied image is placed on the clipboard as
    // real data, which the system owns and which outlives this process by
    // itself. There is nothing left to flush.
    Icons::Shutdown();
    Dpi::ReleaseFonts();
    Theme::Shutdown();
    Utils::ShutdownGdiplus();
    OleUninitialize();

    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
    }

    Logger::Shutdown();
    return 0;
}
