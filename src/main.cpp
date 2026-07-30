#include <windows.h>
#include <commctrl.h>
#include "Resource.h"
#include "Logger.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "Utils.h"
#include "CaptureEngine.h"
#include "FullScreenCapture.h"
#include "WindowCapture.h"
#include "RegionCapture.h"
#include "Overlay.h"
#include "Clipboard.h"
#include "Toast.h"
#include "TrayIcon.h"
#include "HotkeyManager.h"
#include "ScrollCapture.h"
#include "StitchTool.h"

#pragma comment(lib, "comctl32.lib")

static const wchar_t* MAIN_WINDOW_CLASS = L"ScreenshotApp_MainWindowClass";

static void ProcessCaptureOutput(Gdiplus::Bitmap* bmp, const std::wstring& captureType = L"Screenshot") {
    if (!bmp) {
        LOG_ERROR("ProcessCaptureOutput received null bitmap.");
        ToastNotification::Show(L"Capture failed!", 2500);
        return;
    }

    const AppConfig& cfg = SettingsManager::Instance().GetConfig();
    bool copied = false;
    bool saved = false;
    std::wstring savedPath;

    if (cfg.outputAction == OutputAction::ClipboardOnly || cfg.outputAction == OutputAction::Both) {
        copied = CClipboardDataObject::CopyToClipboard(bmp);
    }

    if (cfg.outputAction == OutputAction::SaveOnly || cfg.outputAction == OutputAction::Both) {
        std::wstring fileName = Utils::GenerateTimestampFilename(cfg.filenamePattern, cfg.defaultFormat);
        savedPath = cfg.saveFolderPath + L"\\" + fileName;
        saved = Utils::SaveGdiplusBitmapToFile(bmp, savedPath, cfg.defaultFormat, cfg.imageQuality);
    }

    delete bmp;

    if (cfg.showNotifications) {
        std::wstring msg = captureType;
        if (copied && saved) {
            msg += L" copied & saved!";
        } else if (copied) {
            msg += L" copied to clipboard!";
        } else if (saved) {
            msg += L" saved to file!";
        } else {
            msg += L" processed.";
        }
        ToastNotification::Show(msg, 2500);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_HOTKEY: {
        int id = (int)wParam;
        const AppConfig& cfg = SettingsManager::Instance().GetConfig();

        if (cfg.captureDelayMs > 0) {
            Sleep(cfg.captureDelayMs);
        }

        if (id == HOTKEY_FULLSCREEN_ID) {
            Gdiplus::Bitmap* bmp = FullScreenCapture::Capture(cfg.includeCursor);
            ProcessCaptureOutput(bmp, L"Full screen");
        } else if (id == HOTKEY_WINDOW_ID) {
            HWND hTarget = WindowCapture::SelectTargetWindow();
            Gdiplus::Bitmap* bmp = WindowCapture::Capture(hTarget, cfg.removeWindowShadow, cfg.includeCursor);
            ProcessCaptureOutput(bmp, L"Window capture");
        } else if (id == HOTKEY_REGION_ID) {
            RegionOverlay::Show([](RECT selRect, bool scrollCapture) {
                const AppConfig& cfg = SettingsManager::Instance().GetConfig();
                if (scrollCapture) {
                    Gdiplus::Bitmap* bmp = ScrollCapture::PerformScrollCapture(selRect, 15);
                    ProcessCaptureOutput(bmp, L"Scroll capture");
                } else {
                    Gdiplus::Bitmap* bmp = RegionCapture::Capture(selRect, cfg.includeCursor);
                    ProcessCaptureOutput(bmp, L"Region capture");
                }
            });
        }
        return 0;
    }

    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP) {
            TrayIcon::Instance().ShowMenu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            const AppConfig& cfg = SettingsManager::Instance().GetConfig();
            Gdiplus::Bitmap* bmp = FullScreenCapture::Capture(cfg.includeCursor);
            ProcessCaptureOutput(bmp, L"Full screen");
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        const AppConfig& cfg = SettingsManager::Instance().GetConfig();

        if (id == ID_TRAY_FULLSCREEN) {
            Gdiplus::Bitmap* bmp = FullScreenCapture::Capture(cfg.includeCursor);
            ProcessCaptureOutput(bmp, L"Full screen");
        } else if (id == ID_TRAY_WINDOW) {
            HWND hTarget = WindowCapture::SelectTargetWindow();
            Gdiplus::Bitmap* bmp = WindowCapture::Capture(hTarget, cfg.removeWindowShadow, cfg.includeCursor);
            ProcessCaptureOutput(bmp, L"Window capture");
        } else if (id == ID_TRAY_REGION) {
            RegionOverlay::Show([](RECT selRect, bool scrollCapture) {
                const AppConfig& cfg = SettingsManager::Instance().GetConfig();
                if (scrollCapture) {
                    Gdiplus::Bitmap* bmp = ScrollCapture::PerformScrollCapture(selRect, 15);
                    ProcessCaptureOutput(bmp, L"Scroll capture");
                } else {
                    Gdiplus::Bitmap* bmp = RegionCapture::Capture(selRect, cfg.includeCursor);
                    ProcessCaptureOutput(bmp, L"Region capture");
                }
            });
        } else if (id == ID_TRAY_STITCH) {
            StitchToolDialog::Show(hwnd);
        } else if (id == ID_TRAY_SETTINGS) {
            SettingsDialog::Show(hwnd);
            HotkeyManager::Instance().RegisterAll(hwnd);
        } else if (id == ID_TRAY_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        HotkeyManager::Instance().UnregisterAll(hwnd);
        TrayIcon::Instance().Remove();
        CaptureEngine::Instance().Shutdown();
        OleUninitialize();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Single instance mutex check
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"ScreenshotApp_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"ScreenshotApp is already running in system tray.", L"ScreenshotApp", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    std::wstring appDataFolder = Utils::GetAppDataFolderPath();
    Logger::Instance().Init(appDataFolder + L"\\app.log");
    LOG_INFO("ScreenshotApp starting up...");

    SettingsManager::Instance().Load();

    if (FAILED(OleInitialize(NULL))) {
        LOG_ERROR("OleInitialize failed!");
        return -1;
    }

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    if (!CaptureEngine::Instance().Init()) {
        LOG_ERROR("CaptureEngine initialization failed!");
        MessageBoxW(NULL, L"Failed to initialize Capture Engine / GDI+.", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, MAIN_WINDOW_CLASS, L"ScreenshotApp Hidden Main Window", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL
    );

    if (!hwnd) {
        LOG_ERROR("Failed to create main message window.");
        return -1;
    }

    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    TrayIcon::Instance().Create(hwnd, 1, hIcon, L"Screenshot App - Ready");

    HotkeyManager::Instance().RegisterAll(hwnd);

    LOG_INFO("ScreenshotApp initialization complete. Entering message loop.");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex) CloseHandle(hMutex);
    return (int)msg.wParam;
}
