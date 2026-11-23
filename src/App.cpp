#include "core/Log.h"
#include "io/WorldMesh.h"
#include "ui/DarkTheme.h"
#include "ui/MainWindow.h"
#include "ui/SetupWizard.h"
#include "ui/U1Library.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <shellapi.h>

// Opt in to the version 6 common controls so the tree view and status bar use
// the current visual style instead of the Windows 95 look.
#pragma comment(linker,                                                     \
    "/manifestdependency:\"type='win32' "                                   \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "           \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "          \
    "language='*'\"")

namespace {

double SecondsNow() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value;
        ::QueryPerformanceFrequency(&value);
        return value;
    }();

    LARGE_INTEGER counter;
    ::QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Before any window exists: every one made after this comes up dark.
    dark::Init();

    // Started by the setup again, as administrator, only to copy the plugins.
    int installResult = 0;
    if (RunInstallCommand(installResult)) {
        ::CoUninitialize();
        return installResult;
    }

    // A check of Need for Speed Underground's models, with no window.
    {
        int count = 0;
        LPWSTR* args = ::CommandLineToArgvW(::GetCommandLineW(), &count);
        const bool selfTest = args && count >= 3 && ::lstrcmpiW(args[1], L"--u1-selftest") == 0;
        const std::wstring folder = selfTest ? args[2] : L"";
        if (args) ::LocalFree(args);
        if (selfTest) {
            const int result = RunU1SelfTest(folder);
            ::CoUninitialize();
            return result;
        }
    }

    // A check of a car made into a world model (io/WorldMesh.h CarModelSelfTest), with no window.
    {
        int count = 0;
        LPWSTR* args = ::CommandLineToArgvW(::GetCommandLineW(), &count);
        if (args && count >= 5 && ::lstrcmpiW(args[1], L"--cars-selftest") == 0) {
            const std::wstring car = args[3];
            const int result = io::CarModelSelfTest(args[2], std::string(car.begin(), car.end()), args[4]);
            ::LocalFree(args);
            ::CoUninitialize();
            return result;
        }
        if (args) ::LocalFree(args);
    }

    MainWindow window;
    if (!window.Create(instance, showCommand)) {
        ::CoUninitialize();
        return 1;
    }

    // The viewport is animated, so the loop renders whenever there is nothing
    // else to do rather than waiting on messages. WaitMessage is used while the
    // window is hidden so a minimised editor does not spin the GPU.
    double previous = SecondsNow();
    MSG message{};

    while (true) {
        if (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) break;


            // Blender's way: the wheel zooms the view the cursor is over,
            // without a click to give it the focus first.
            if (message.message == WM_MOUSEWHEEL) {
                POINT cursor{ (short)LOWORD(message.lParam), (short)HIWORD(message.lParam) };
                RECT view{};
                ::GetWindowRect(window.ViewportHandle(), &view);
                if (::PtInRect(&view, cursor)) message.hwnd = window.ViewportHandle();
            }

            HWND browser = window.TextureDialog();
            if (browser && ::IsDialogMessageW(browser, &message)) continue;

            // A text field keeps its own keys: Del, Ctrl+C and Ctrl+V there edit
            // the text, not the scene.
            wchar_t focusClass[16] = {};
            ::GetClassNameW(::GetFocus(), focusClass, 16);
            const bool typing = ::lstrcmpiW(focusClass, L"Edit") == 0 || window.WantsTextInput();

            if (typing || !::TranslateAcceleratorW(window.Handle(), window.Accelerators(), &message)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            continue;
        }

        if (::IsIconic(window.Handle())) {
            ::WaitMessage();
            previous = SecondsNow();
            continue;
        }

        const double now = SecondsNow();
        // Clamp the step so a stall (a long parse, a dragged window) does not
        // teleport the camera on the next frame.
        const float delta = (float)std::min(now - previous, 0.1);
        previous = now;

        window.Tick(delta);
    }

    window.Destroy();
    ::CoUninitialize();
    return (int)message.wParam;
}
