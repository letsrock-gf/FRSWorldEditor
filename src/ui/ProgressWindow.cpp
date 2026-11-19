#include "ui/ProgressWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>

#pragma comment(lib, "uxtheme.lib")

namespace {

constexpr wchar_t kClassName[] = L"FRSProgressWindow";

LRESULT CALLBACK ProgressProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CLOSE: return 0;    // only the work closes it
        case WM_CTLCOLORSTATIC: {
            // The label on the window's own dark background.
            HDC dc = (HDC)wParam;
            ::SetTextColor(dc, RGB(220, 226, 236));
            ::SetBkColor(dc, RGB(30, 35, 45));
            static HBRUSH brush = ::CreateSolidBrush(RGB(30, 35, 45));
            return (LRESULT)brush;
        }
        default: return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

void RegisterOnce(HINSTANCE instance) {
    static bool done = false;
    if (done) return;
    done = true;
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_PROGRESS_CLASS };
    ::InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = ProgressProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_WAIT);
    wc.hbrBackground = ::CreateSolidBrush(RGB(30, 35, 45));
    wc.lpszClassName = kClassName;
    ::RegisterClassExW(&wc);
}

} // namespace

ProgressWindow::ProgressWindow(HWND owner, const std::wstring& title, int steps)
    : owner_(owner), steps_(std::max(1, steps)) {
    HINSTANCE instance = (HINSTANCE)::GetModuleHandleW(nullptr);
    RegisterOnce(instance);
    const UINT dpi = owner ? ::GetDpiForWindow(owner) : 96;
    const auto px = [dpi](int v) { return ::MulDiv(v, (int)dpi, 96); };
    const int width = px(460), height = px(130);
    RECT area{};
    if (owner) ::GetWindowRect(owner, &area);
    else ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    const int x = area.left + (area.right - area.left - width) / 2;
    const int y = area.top + (area.bottom - area.top - height) / 2;
    window_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW, kClassName, title.c_str(),
                                WS_POPUP | WS_CAPTION | WS_VISIBLE, x, y, width, height, owner, nullptr, instance,
                                nullptr);
    if (!window_) return;
    const BOOL dark = TRUE;
    ::DwmSetWindowAttribute(window_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    NONCLIENTMETRICSW metrics{ sizeof(metrics) };
    ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
    RECT client{};
    ::GetClientRect(window_, &client);
    label_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, px(16),
                               px(14), client.right - px(32), px(22), window_, nullptr, instance, nullptr);
    bar_ = ::CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, px(16), px(44),
                             client.right - px(32), px(18), window_, nullptr, instance, nullptr);
    ::SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    ::SetWindowTheme(bar_, L"", L"");     // so the colours below apply
    ::SendMessageW(bar_, PBM_SETRANGE32, 0, steps_ * 100);
    ::SendMessageW(bar_, PBM_SETBARCOLOR, 0, (LPARAM)RGB(54, 150, 219));
    ::SendMessageW(bar_, PBM_SETBKCOLOR, 0, (LPARAM)RGB(45, 52, 66));
    Pump();
}

ProgressWindow::~ProgressWindow() { Close(); }

void ProgressWindow::Close() {
    if (window_) {
        ::DestroyWindow(window_);
        window_ = nullptr;
        if (owner_) ::SetForegroundWindow(owner_);
    }
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
}

void ProgressWindow::Step(const std::wstring& text) {
    if (!window_) return;
    ::SetWindowTextW(label_, text.c_str());
    ::SendMessageW(bar_, PBM_SETPOS, (WPARAM)(std::min(steps_, done_) * 100 + 30), 0);
    ++done_;
    Pump();
}

void ProgressWindow::Pump() {
    if (!window_) return;
    // Painted now, without waiting for the message loop.
    ::RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    // Its own messages only (moving it, painting); the editor's stay queued.
    MSG message;
    while (::PeekMessageW(&message, window_, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    // Looking at the queue is what tells Windows the editor is alive.
    ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
}
