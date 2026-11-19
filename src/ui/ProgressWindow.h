#pragma once

#include <windows.h>

#include <string>

// A small window with a progress bar and the step being worked on, for long
// work done on the editor's own thread (a bake, a write to the game).
//
// The editor's frame is drawn by ImGui from its message loop, which does not
// run while such work does, so without this the window just looks frozen.
// This one is a native window that paints itself at every step. Each step also
// peeks at the thread's messages, so Windows does not mark the editor as not
// responding -- without handing any of them to the editor, which must not
// redraw or react to clicks while the work runs.
class ProgressWindow {
public:
    // `steps`: how many Step() calls the work makes, for the bar.
    ProgressWindow(HWND owner, const std::wstring& title, int steps);
    ~ProgressWindow();
    ProgressWindow(const ProgressWindow&) = delete;
    ProgressWindow& operator=(const ProgressWindow&) = delete;

    // The next step starts: the bar moves on and shows what it is.
    void Step(const std::wstring& text);
    // Repaints and keeps the editor answering; for long steps of their own.
    void Pump();
    // Takes it down now (before a message box, say). Also done by the destructor.
    void Close();

private:
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HWND label_ = nullptr;
    HWND bar_ = nullptr;
    HFONT font_ = nullptr;
    int steps_ = 1;
    int done_ = 0;
};
