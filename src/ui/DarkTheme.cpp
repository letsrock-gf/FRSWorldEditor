#include "ui/DarkTheme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace dark {

namespace {

constexpr COLORREF kBack   = RGB(32, 33, 36);     // windows and dialogs
constexpr COLORREF kPanel  = RGB(43, 45, 48);     // bars
constexpr COLORREF kField  = RGB(24, 25, 27);     // lists, trees, text fields
constexpr COLORREF kText   = RGB(222, 222, 222);
constexpr COLORREF kDim    = RGB(130, 130, 130);
constexpr COLORREF kAccent = RGB(47, 101, 202);
constexpr COLORREF kHot    = RGB(62, 64, 68);
constexpr COLORREF kEdge   = RGB(70, 72, 76);

bool g_enabled = false;

HBRUSH Brush(COLORREF colour) {
    // A handful of colours, each made once and kept for the life of the program.
    static struct { COLORREF colour; HBRUSH brush; } cache[16];
    for (auto& entry : cache) {
        if (entry.brush && entry.colour == colour) return entry.brush;
        if (!entry.brush) {
            entry.colour = colour;
            entry.brush = ::CreateSolidBrush(colour);
            return entry.brush;
        }
    }
    return (HBRUSH)::GetStockObject(BLACK_BRUSH);
}

// --- the system colours, as this program sees them ---------------------------------

using GetSysColorFn      = DWORD (WINAPI*)(int);
using GetSysColorBrushFn = HBRUSH (WINAPI*)(int);
GetSysColorFn      g_realColour = nullptr;
GetSysColorBrushFn g_realBrush  = nullptr;

bool DarkColour(int index, COLORREF& colour) {
    switch (index) {
        case COLOR_WINDOW:                                   colour = kField; return true;
        case COLOR_BTNFACE: case COLOR_MENU: case COLOR_MENUBAR:
        case COLOR_SCROLLBAR: case COLOR_APPWORKSPACE:       colour = kBack; return true;
        case COLOR_WINDOWTEXT: case COLOR_BTNTEXT: case COLOR_MENUTEXT:
        case COLOR_CAPTIONTEXT: case COLOR_INFOTEXT:         colour = kText; return true;
        case COLOR_GRAYTEXT:                                 colour = kDim; return true;
        case COLOR_HIGHLIGHT: case COLOR_HOTLIGHT: case COLOR_MENUHILIGHT:
                                                             colour = kAccent; return true;
        case COLOR_HIGHLIGHTTEXT:                            colour = RGB(255, 255, 255); return true;
        case COLOR_BTNSHADOW: case COLOR_WINDOWFRAME:
        case COLOR_ACTIVEBORDER: case COLOR_INACTIVEBORDER:  colour = kEdge; return true;
        case COLOR_3DDKSHADOW:                               colour = RGB(12, 12, 12); return true;
        case COLOR_BTNHIGHLIGHT:                             colour = kHot; return true;
        case COLOR_3DLIGHT: case COLOR_INFOBK:               colour = kPanel; return true;
        default:                                             return false;
    }
}

DWORD WINAPI HookedGetSysColor(int index) {
    COLORREF colour = 0;
    if (DarkColour(index, colour)) return colour;
    return g_realColour ? g_realColour(index) : 0;
}

HBRUSH WINAPI HookedGetSysColorBrush(int index) {
    COLORREF colour = 0;
    if (DarkColour(index, colour)) return Brush(colour);
    return g_realBrush ? g_realBrush(index) : nullptr;
}

// Points this executable's own import of `name` at `replacement`. Only calls
// made from the editor's code (and the property grid built into it) change;
// the system's controls keep asking Windows.
void PatchImport(const char* dll, const char* name, void* replacement) {
    BYTE* module = (BYTE*)::GetModuleHandleW(nullptr);
    const auto* dos = (const IMAGE_DOS_HEADER*)module;
    const auto* nt = (const IMAGE_NT_HEADERS*)(module + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return;

    for (auto* entry = (IMAGE_IMPORT_DESCRIPTOR*)(module + imports.VirtualAddress); entry->Name; ++entry) {
        if (::_stricmp((const char*)module + entry->Name, dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(module + (entry->OriginalFirstThunk ? entry->OriginalFirstThunk
                                                                              : entry->FirstThunk));
        auto* slots = (IMAGE_THUNK_DATA*)(module + entry->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* import = (const IMAGE_IMPORT_BY_NAME*)(module + names->u1.AddressOfData);
            if (std::strcmp((const char*)import->Name, name) != 0) continue;
            DWORD protection = 0;
            if (!::VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &protection)) continue;
            slots->u1.Function = (ULONG_PTR)replacement;
            ::VirtualProtect(&slots->u1.Function, sizeof(void*), protection, &protection);
        }
    }
}

// --- uxtheme's unnamed exports ---------------------------------------------------------

using AllowDarkForWindowFn = BOOL (WINAPI*)(HWND, BOOL);
AllowDarkForWindowFn g_allowForWindow = nullptr;

void DarkControl(HWND control, const wchar_t* theme) {
    if (g_allowForWindow) g_allowForWindow(control, TRUE);
    ::SetWindowTheme(control, theme, nullptr);
}

// --- the menu bar ------------------------------------------------------------------------
// Windows sends these to a window so that it may draw its own menu bar; they
// are not in the SDK headers. The layouts are the ones everybody uses.
constexpr UINT kUahDrawMenu     = 0x0091;
constexpr UINT kUahDrawMenuItem = 0x0092;

struct UahMenu { HMENU menu; HDC dc; DWORD flags; };
union UahMenuItemMetrics {
    struct { DWORD cx, cy; } bar[2];
    struct { DWORD cx, cy; } popup[4];
};
struct UahMenuPopupMetrics { DWORD cx[4]; DWORD updateMaxWidths : 2; };
struct UahMenuItem { int position; UahMenuItemMetrics itemMetrics; UahMenuPopupMetrics popupMetrics; };
struct UahDrawMenuItem { DRAWITEMSTRUCT draw; UahMenu menu; UahMenuItem item; };

HFONT MenuFont(HWND window) {
    static HFONT font = nullptr;
    static UINT fontDpi = 0;
    const UINT dpi = ::GetDpiForWindow(window);
    if (!font || fontDpi != dpi) {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
            if (font) ::DeleteObject(font);
            font = ::CreateFontIndirectW(&metrics.lfMenuFont);
            fontDpi = dpi;
        }
    }
    return font;
}

void PaintMenuBar(HWND window, const UahMenu& menu) {
    MENUBARINFO info{};
    info.cbSize = sizeof(info);
    RECT whole{};
    if (!::GetMenuBarInfo(window, OBJID_MENU, 0, &info) || !::GetWindowRect(window, &whole)) return;
    RECT bar = info.rcBar;
    ::OffsetRect(&bar, -whole.left, -whole.top);
    ::FillRect(menu.dc, &bar, Brush(kPanel));
}

void DrawMenuItem(HWND window, const UahDrawMenuItem& item) {
    wchar_t text[128] = {};
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STRING;
    info.dwTypeData = text;
    info.cch = 127;
    ::GetMenuItemInfoW(item.menu.menu, (UINT)item.item.position, TRUE, &info);

    const UINT state = item.draw.itemState;
    const bool hot = (state & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
    const bool off = (state & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE)) != 0;
    ::FillRect(item.menu.dc, &item.draw.rcItem, Brush(hot ? kHot : kPanel));

    HGDIOBJ previous = ::SelectObject(item.menu.dc, MenuFont(window));
    ::SetBkMode(item.menu.dc, TRANSPARENT);
    ::SetTextColor(item.menu.dc, off ? kDim : kText);
    RECT area = item.draw.rcItem;
    ::DrawTextW(item.menu.dc, text, -1, &area,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((state & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
    if (previous) ::SelectObject(item.menu.dc, previous);
}

// The system leaves a light line between the menu bar and the client area.
void HideMenuLine(HWND window) {
    if (!::GetMenu(window)) return;
    RECT client{}, whole{};
    ::GetClientRect(window, &client);
    ::MapWindowPoints(window, nullptr, (POINT*)&client, 2);
    ::GetWindowRect(window, &whole);
    ::OffsetRect(&client, -whole.left, -whole.top);
    RECT line = client;
    line.bottom = line.top;
    line.top -= 1;
    if (HDC dc = ::GetWindowDC(window)) {
        ::FillRect(dc, &line, Brush(kPanel));
        ::ReleaseDC(window, dc);
    }
}

// --- controls nothing themes ------------------------------------------------------------

HFONT FontOf(HWND control) {
    HFONT font = (HFONT)::SendMessageW(control, WM_GETFONT, 0, 0);
    return font ? font : (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
}

LRESULT CALLBACK StatusProc(HWND bar, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = ::BeginPaint(bar, &paint);
            RECT client{};
            ::GetClientRect(bar, &client);
            ::FillRect(dc, &client, Brush(kPanel));

            HGDIOBJ previous = ::SelectObject(dc, FontOf(bar));
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, kText);
            const int parts = (int)::SendMessageW(bar, SB_GETPARTS, 0, 0);
            for (int i = 0; i < parts; ++i) {
                RECT part{};
                if (!::SendMessageW(bar, SB_GETRECT, (WPARAM)i, (LPARAM)&part)) continue;
                const int length = LOWORD(::SendMessageW(bar, SB_GETTEXTLENGTHW, (WPARAM)i, 0));
                std::wstring text((size_t)length + 1, L'\0');
                ::SendMessageW(bar, SB_GETTEXTW, (WPARAM)i, (LPARAM)text.data());
                if (i) {
                    RECT edge{ part.left, part.top + 3, part.left + 1, part.bottom - 3 };
                    ::FillRect(dc, &edge, Brush(kEdge));
                }
                part.left += 6;
                ::DrawTextW(dc, text.c_str(), length, &part, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            if (previous) ::SelectObject(dc, previous);
            ::EndPaint(bar, &paint);
            return 0;
        }
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(bar, StatusProc, id);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(bar, message, wParam, lParam);
}

LRESULT CALLBACK TabProc(HWND tabs, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = ::BeginPaint(tabs, &paint);
            RECT client{};
            ::GetClientRect(tabs, &client);
            ::FillRect(dc, &client, Brush(kBack));

            HGDIOBJ previous = ::SelectObject(dc, FontOf(tabs));
            ::SetBkMode(dc, TRANSPARENT);
            const int count = TabCtrl_GetItemCount(tabs);
            const int chosen = TabCtrl_GetCurSel(tabs);
            for (int i = 0; i < count; ++i) {
                RECT tab{};
                if (!TabCtrl_GetItemRect(tabs, i, &tab)) continue;
                wchar_t text[96] = {};
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = text;
                item.cchTextMax = 95;
                TabCtrl_GetItem(tabs, i, &item);
                ::FillRect(dc, &tab, Brush(i == chosen ? kHot : kPanel));
                if (i == chosen) {
                    RECT mark{ tab.left, tab.bottom - 2, tab.right, tab.bottom };
                    ::FillRect(dc, &mark, Brush(kAccent));
                }
                ::SetTextColor(dc, i == chosen ? RGB(255, 255, 255) : kText);
                ::DrawTextW(dc, text, -1, &tab, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            if (previous) ::SelectObject(dc, previous);
            ::EndPaint(tabs, &paint);
            return 0;
        }
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(tabs, TabProc, id);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(tabs, message, wParam, lParam);
}

LRESULT CALLBACK ToolbarProc(HWND bar, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_ERASEBKGND) {
        RECT client{};
        ::GetClientRect(bar, &client);
        ::FillRect((HDC)wParam, &client, Brush(kPanel));
        return 1;
    }
    if (message == WM_NCDESTROY) ::RemoveWindowSubclass(bar, ToolbarProc, id);
    return ::DefSubclassProc(bar, message, wParam, lParam);
}

// A list's column header asks the list how to draw itself; only the text
// colour has to be said, the dark background comes with its theme.
LRESULT CALLBACK ListProc(HWND list, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NOTIFY) {
        const auto* header = (const NMHDR*)lParam;
        if (header->code == NM_CUSTOMDRAW && header->hwndFrom == ListView_GetHeader(list)) {
            const auto* draw = (const NMCUSTOMDRAW*)lParam;
            if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                ::SetTextColor(draw->hdc, kText);
                return CDRF_DODEFAULT;
            }
        }
    }
    if (message == WM_NCDESTROY) ::RemoveWindowSubclass(list, ListProc, id);
    return ::DefSubclassProc(list, message, wParam, lParam);
}

bool ClassIs(HWND window, const wchar_t* name) {
    wchar_t text[64] = {};
    ::GetClassNameW(window, text, 64);
    return ::lstrcmpiW(text, name) == 0;
}

// A button that looks like a push button: one proper, or a check box or
// radio button with the push-like style.
bool PushButton(HWND button) {
    const LONG style = ::GetWindowLongW(button, GWL_STYLE);
    const LONG type = style & BS_TYPEMASK;
    return type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || (style & BS_PUSHLIKE) != 0;
}

// The system's dark button theme took in dialogs and not in the main window,
// so push buttons are drawn here, the same everywhere. The button still does
// everything else itself; it only asks, through NM_CUSTOMDRAW, to be painted.
void PaintButton(HWND button, const NMCUSTOMDRAW& draw) {
    const bool disabled = (draw.uItemState & CDIS_DISABLED) != 0;
    const bool pressed  = (draw.uItemState & CDIS_SELECTED) != 0;
    const bool hot      = (draw.uItemState & CDIS_HOT) != 0;
    const bool checked  = ::SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;

    RECT area = draw.rc;
    ::FillRect(draw.hdc, &area, Brush(kBack));
    const COLORREF face = pressed ? kAccent : checked ? RGB(74, 78, 86) : hot ? kHot : kPanel;
    HPEN pen = ::CreatePen(PS_SOLID, 1, (draw.uItemState & CDIS_FOCUS) ? kAccent : kEdge);
    HGDIOBJ oldPen = ::SelectObject(draw.hdc, pen);
    HGDIOBJ oldBrush = ::SelectObject(draw.hdc, Brush(face));
    ::RoundRect(draw.hdc, area.left, area.top, area.right, area.bottom, 6, 6);
    ::SelectObject(draw.hdc, oldBrush);
    ::SelectObject(draw.hdc, oldPen);
    ::DeleteObject(pen);

    wchar_t text[160] = {};
    ::GetWindowTextW(button, text, 159);
    HGDIOBJ oldFont = ::SelectObject(draw.hdc, FontOf(button));
    ::SetBkMode(draw.hdc, TRANSPARENT);
    ::SetTextColor(draw.hdc, disabled ? kDim : kText);
    ::DrawTextW(draw.hdc, text, -1, &area, DT_CENTER | DT_SINGLELINE | DT_VCENTER |
                ((draw.uItemState & CDIS_SHOWKEYBOARDCUES) ? 0 : DT_HIDEPREFIX));
    if (oldFont) ::SelectObject(draw.hdc, oldFont);
}

// A control that holds controls of its own -- the property grid -- answers
// their colour questions itself, with the system's light ones. Here it is
// given dark ones.
LRESULT CALLBACK ContainerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    switch (message) {
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            ::SetTextColor((HDC)wParam, kText);
            ::SetBkColor((HDC)wParam, kBack);
            return (LRESULT)Brush(kBack);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            ::SetTextColor((HDC)wParam, kText);
            ::SetBkColor((HDC)wParam, kField);
            return (LRESULT)Brush(kField);
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(window, ContainerProc, id);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

BOOL CALLBACK ThemeChild(HWND control, LPARAM) {
    if (ClassIs(control, WC_TREEVIEWW)) {
        DarkControl(control, L"DarkMode_Explorer");
        TreeView_SetBkColor(control, kField);
        TreeView_SetTextColor(control, kText);
        TreeView_SetLineColor(control, kDim);
    } else if (ClassIs(control, WC_LISTVIEWW)) {
        DarkControl(control, L"DarkMode_Explorer");
        ListView_SetBkColor(control, kField);
        ListView_SetTextBkColor(control, kField);
        ListView_SetTextColor(control, kText);
        if (HWND header = ListView_GetHeader(control)) DarkControl(header, L"DarkMode_ItemsView");
        ::SetWindowSubclass(control, ListProc, 1, 0);
    } else if (ClassIs(control, WC_BUTTONW)) {
        // A themed check box or radio button draws its label in the theme's
        // black whatever it is told; unthemed, it takes the colour given.
        const LONG style = ::GetWindowLongW(control, GWL_STYLE);
        const LONG type = style & BS_TYPEMASK;
        const bool labelled = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE ||
                              type == BS_AUTO3STATE || type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON ||
                              type == BS_GROUPBOX;
        if (labelled && !(style & BS_PUSHLIKE)) ::SetWindowTheme(control, L"", L"");
        else                                    DarkControl(control, L"DarkMode_Explorer");
    } else if (ClassIs(control, WC_EDITW) || ClassIs(control, WC_COMBOBOXW)) {
        DarkControl(control, L"DarkMode_CFD");
    } else if (ClassIs(control, STATUSCLASSNAMEW)) {
        ::SetWindowSubclass(control, StatusProc, 1, 0);
    } else if (ClassIs(control, WC_TABCONTROLW)) {
        ::SetWindowSubclass(control, TabProc, 1, 0);
    } else if (ClassIs(control, TOOLBARCLASSNAMEW)) {
        ::SetWindowSubclass(control, ToolbarProc, 1, 0);
    } else if (ClassIs(control, WC_SCROLLBARW) || ClassIs(control, UPDOWN_CLASSW) || ClassIs(control, WC_LISTBOXW)) {
        DarkControl(control, L"DarkMode_Explorer");
    } else if (!ClassIs(control, WC_STATICW) && ::GetWindow(control, GW_CHILD)) {
        ::SetWindowSubclass(control, ContainerProc, 2, 0);
    }
    return TRUE;
}

// --- a top-level window or a dialog --------------------------------------------------------

LRESULT CALLBACK TopProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    switch (message) {
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORMSGBOX:
            ::SetTextColor((HDC)wParam, kText);
            ::SetBkColor((HDC)wParam, kBack);
            return (LRESULT)Brush(kBack);

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            ::SetTextColor((HDC)wParam, kText);
            ::SetBkColor((HDC)wParam, kField);
            return (LRESULT)Brush(kField);

        case WM_ERASEBKGND: {
            RECT client{};
            ::GetClientRect(window, &client);
            ::FillRect((HDC)wParam, &client, Brush(kBack));
            return 1;
        }

        case WM_NOTIFY: {
            const auto* header = (const NMHDR*)lParam;
            if (header->code == NM_CUSTOMDRAW && ClassIs(header->hwndFrom, WC_BUTTONW) &&
                PushButton(header->hwndFrom)) {
                const auto* draw = (const NMCUSTOMDRAW*)lParam;
                if (draw->dwDrawStage == CDDS_PREPAINT) {
                    PaintButton(header->hwndFrom, *draw);
                    return CDRF_SKIPDEFAULT;
                }
            }
            if (header->code == NM_CUSTOMDRAW && ClassIs(header->hwndFrom, TOOLBARCLASSNAMEW)) {
                auto* draw = (NMTBCUSTOMDRAW*)lParam;
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    draw->clrText = kText;
                    draw->clrTextHighlight = RGB(255, 255, 255);
                    draw->clrBtnFace = kPanel;
                    draw->clrBtnHighlight = kHot;
                    draw->clrHighlightHotTrack = kHot;
                    // The theme paints a pressed, hot or chosen button light,
                    // under text that is now light too: its background is ours.
                    const UINT state = draw->nmcd.uItemState;
                    if (state & (CDIS_CHECKED | CDIS_SELECTED | CDIS_HOT)) {
                        ::FillRect(draw->nmcd.hdc, &draw->nmcd.rc,
                                   Brush((state & (CDIS_CHECKED | CDIS_SELECTED)) ? RGB(52, 78, 128) : kHot));
                        return TBCDRF_USECDCOLORS | TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOMARK;
                    }
                    return TBCDRF_USECDCOLORS;
                }
            }
            break;
        }

        // A control made after the window was adopted.
        case WM_PARENTNOTIFY: {
            if (LOWORD(wParam) == WM_CREATE && lParam) {
                ThemeChild((HWND)lParam, 0);
                ::EnumChildWindows((HWND)lParam, ThemeChild, 0);
            }
            break;
        }

        case kUahDrawMenu:
            if (lParam) { PaintMenuBar(window, *(const UahMenu*)lParam); return 0; }
            break;
        case kUahDrawMenuItem:
            if (lParam) { DrawMenuItem(window, *(const UahDrawMenuItem*)lParam); return 0; }
            break;

        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
            HideMenuLine(window);
            return result;
        }

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(window, TopProc, id);
            break;

        default:
            break;
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

BOOL CALLBACK HasForeignChild(HWND child, LPARAM found) {
    // The system's own file dialogs, and wizards (dialogs inside a dialog),
    // are left to themselves.
    if (ClassIs(child, L"DirectUIHWND") || ClassIs(child, L"DUIViewWndClassName") || ClassIs(child, L"#32770")) {
        *(bool*)found = true;
        return FALSE;
    }
    return TRUE;
}

LRESULT CALLBACK ActivationHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_ACTIVATE) Adopt((HWND)wParam);
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

// A window that is shown without being activated -- the editor started behind
// another program -- would stay light until clicked. A dialog is taken as soon
// as its controls exist, so it never shows light first.
LRESULT CALLBACK ShownHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0) {
        const auto* sent = (const CWPRETSTRUCT*)lParam;
        if (sent->message == WM_INITDIALOG || (sent->message == WM_SHOWWINDOW && sent->wParam)) Adopt(sent->hwnd);
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace

bool Enabled() { return g_enabled; }

void Init() {
    wchar_t light[8] = {};
    if (::GetEnvironmentVariableW(L"FRS_LIGHT", light, 8) && light[0] == L'1') return;
    g_enabled = true;

    if (HMODULE user = ::GetModuleHandleW(L"user32.dll")) {
        g_realColour = (GetSysColorFn)::GetProcAddress(user, "GetSysColor");
        g_realBrush  = (GetSysColorBrushFn)::GetProcAddress(user, "GetSysColorBrush");
    }
    PatchImport("user32.dll", "GetSysColor", (void*)HookedGetSysColor);
    PatchImport("user32.dll", "GetSysColorBrush", (void*)HookedGetSysColorBrush);

    // Dark popup menus and dark themes for controls: ordinals 135
    // (SetPreferredAppMode, 2 = always dark), 136 (FlushMenuThemes) and 133
    // (AllowDarkModeForWindow), there since Windows 10 1903.
    if (HMODULE theme = ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetAppModeFn = int (WINAPI*)(int);
        using FlushFn = void (WINAPI*)();
        if (auto setMode = (SetAppModeFn)::GetProcAddress(theme, MAKEINTRESOURCEA(135))) setMode(2);
        if (auto flush = (FlushFn)::GetProcAddress(theme, MAKEINTRESOURCEA(136))) flush();
        g_allowForWindow = (AllowDarkForWindowFn)::GetProcAddress(theme, MAKEINTRESOURCEA(133));
    }

    ::SetWindowsHookExW(WH_CBT, ActivationHook, nullptr, ::GetCurrentThreadId());
    ::SetWindowsHookExW(WH_CALLWNDPROCRET, ShownHook, nullptr, ::GetCurrentThreadId());
}

void Adopt(HWND window) {
    if (!g_enabled || !window) return;
    if (::GetWindowLongW(window, GWL_STYLE) & WS_CHILD) return;
    if (::GetPropW(window, L"FrsDark")) return;

    const bool dialog = ClassIs(window, L"#32770");
    if (!dialog && !ClassIs(window, L"FRSWorldEditor.MainWindow")) return;
    if (dialog) {
        bool foreign = false;
        ::EnumChildWindows(window, HasForeignChild, (LPARAM)&foreign);
        if (foreign) return;
    }
    ::SetPropW(window, L"FrsDark", (HANDLE)1);

    const BOOL on = TRUE;
    ::DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
    if (g_allowForWindow) g_allowForWindow(window, TRUE);

    ::SetWindowSubclass(window, TopProc, 1, 0);
    ::EnumChildWindows(window, ThemeChild, 0);
    ::RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    if (::GetMenu(window)) ::DrawMenuBar(window);
}

} // namespace dark
