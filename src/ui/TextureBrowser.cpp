#include "ui/TextureBrowser.h"

#include "core/Log.h"
#include "nfs/TextureDecode.h"
#include "ui/Resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace {

// The tabs of the list. A texture's city comes from its name: the editor names the ones it adds FRS_<...>,
// and the ones of a converted city FRS_<game>_<...>. A new city = one name here and one prefix in CityOf.
const wchar_t* const kCities[] = { L"All", L"Palmont", L"Bayview", L"Added" };

int CityOf(const std::string& name) {
    if (name.compare(0, 7, "FRS_U2_") == 0) return 2;   // Bayview, from Underground 2
    if (name.compare(0, 4, "FRS_") == 0) return 3;      // added in the editor, of no city
    return 1;                                           // shipped with the game
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(),
                                             nullptr, 0);
    std::wstring out((size_t)needed, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), needed);
    return out;
}

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    ::_vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

// Writes a 32-bit bottom-up BMP. Uncompressed, so no encoder is needed and the
// file opens in anything.
bool WriteBmp(const std::wstring& path, const std::vector<u32>& pixels,
              u32 width, u32 height) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;

    const u32 imageBytes = width * height * 4;

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType    = 0x4D42;   // "BM"
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize    = fileHeader.bfOffBits + imageBytes;

    BITMAPINFOHEADER info{};
    info.biSize        = sizeof(info);
    info.biWidth       = (LONG)width;
    info.biHeight      = (LONG)height;   // positive: rows run bottom-up
    info.biPlanes      = 1;
    info.biBitCount    = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage   = imageBytes;

    std::fwrite(&fileHeader, sizeof(fileHeader), 1, file);
    std::fwrite(&info, sizeof(info), 1, file);
    for (u32 y = 0; y < height; ++y)
        std::fwrite(&pixels[(size_t)(height - 1 - y) * width], 4, width, file);

    std::fclose(file);
    return true;
}

} // namespace

void TextureBrowser::Show(HINSTANCE instance, HWND parent, const nfs::World* world) {
    world_ = world;

    if (!dialog_) {
        dialog_ = ::CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_TEXTURES),
                                       parent, &TextureBrowser::DialogProc,
                                       (LPARAM)this);
        if (!dialog_) {
            LOG_ERROR("could not create the texture browser");
            return;
        }
        Layout();   // the tabs are made in OnInit, not in the dialog template: they get their place here
    }

    Populate();
    ::ShowWindow(dialog_, SW_SHOW);
    ::SetForegroundWindow(dialog_);
}

void TextureBrowser::SetFilter(std::vector<u32> hashes) {
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    if (hashes == filter_) return;
    filter_ = std::move(hashes);
    if (dialog_) Populate();
}

void TextureBrowser::SetWorld(const nfs::World* world) {
    world_ = world;
    if (dialog_) Populate();
}

void TextureBrowser::Close() {
    if (dialog_) {
        ::DestroyWindow(dialog_);
        dialog_ = nullptr;
        list_   = nullptr;
        tabs_   = nullptr;
    }
}

INT_PTR CALLBACK TextureBrowser::DialogProc(HWND dialog, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
    auto* self = (TextureBrowser*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
        case WM_INITDIALOG:
            ::SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)lParam);
            ((TextureBrowser*)lParam)->OnInit(dialog);
            return TRUE;

        case WM_NOTIFY: {
            if (!self) break;
            auto* header = (NMHDR*)lParam;
            if (header->idFrom == IDC_TEX_LIST && header->code == LVN_ITEMCHANGED) {
                auto* change = (NMLISTVIEW*)lParam;
                if (change->uNewState & LVIS_SELECTED) self->OnSelectionChanged();
            }
            if (header->hwndFrom == self->tabs_ && header->code == TCN_SELCHANGE) self->Populate();
            return TRUE;
        }

        case WM_DRAWITEM:
            if (self && wParam == IDC_TEX_PREVIEW) {
                self->DrawPreview(*(DRAWITEMSTRUCT*)lParam);
                return TRUE;
            }
            break;

        case WM_COMMAND:
            if (!self) break;
            switch (LOWORD(wParam)) {
                case IDC_TEX_SAVE: self->SaveSelected(); return TRUE;
                case IDC_TEX_REPLACE:
                case IDC_TEX_RESTORE:
                    if (const nfs::Texture* chosen = self->Selected())
                        if (self->onReplace) self->onReplace(chosen->hash, LOWORD(wParam) == IDC_TEX_RESTORE);
                    return TRUE;
                case IDC_TEX_SEARCH:
                    if (HIWORD(wParam) == EN_CHANGE) self->Populate();
                    return TRUE;
                case IDC_TEX_ZOOM:
                    ::InvalidateRect(::GetDlgItem(dialog, IDC_TEX_PREVIEW), nullptr, TRUE);
                    return TRUE;
                case IDCANCEL:
                    ::ShowWindow(dialog, SW_HIDE);
                    return TRUE;
                default: break;
            }
            break;

        case WM_SIZE:
            if (self) self->Layout();
            return TRUE;

        case WM_GETMINMAXINFO: {
            auto* info = (MINMAXINFO*)lParam;
            info->ptMinTrackSize.x = ::MulDiv(560, (int)::GetDpiForWindow(dialog), 96);
            info->ptMinTrackSize.y = ::MulDiv(420, (int)::GetDpiForWindow(dialog), 96);
            return TRUE;
        }

        case WM_CLOSE:
            ::ShowWindow(dialog, SW_HIDE);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

void TextureBrowser::OnInit(HWND dialog) {
    dialog_ = dialog;
    list_   = ::GetDlgItem(dialog, IDC_TEX_LIST);

    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ::SendDlgItemMessageW(dialog, IDC_TEX_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search by name");

    tabs_ = ::CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
                              0, 0, 0, 0, dialog, nullptr, nullptr, nullptr);
    ::SendMessageW(tabs_, WM_SETFONT, ::SendMessageW(dialog, WM_GETFONT, 0, 0), TRUE);
    for (int i = 0; i < (int)std::size(kCities); ++i) {
        TCITEMW tab{};
        tab.mask    = TCIF_TEXT;
        tab.pszText = (LPWSTR)kCities[i];
        TabCtrl_InsertItem(tabs_, i, &tab);
    }

    struct { const wchar_t* title; int width; } columns[] = {
        { L"Name",   150 },
        { L"Size",    62 },
        { L"Format",  52 },
        { L"Used",    42 },
    };

    for (int i = 0; i < (int)std::size(columns); ++i) {
        LVCOLUMNW column{};
        column.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText  = (LPWSTR)columns[i].title;
        column.cx       = ::MulDiv(columns[i].width, (int)::GetDpiForWindow(dialog), 96);
        column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }
}

void TextureBrowser::Populate() {
    if (!list_) return;

    // The texture that was chosen stays chosen when the list is made again:
    // after replacing one, it is the one to look at.
    const nfs::Texture* before = Selected();
    const u32 chosen = before ? before->hash : 0;

    ListView_DeleteAllItems(list_);
    textures_.clear();
    pixels_.clear();
    pixelWidth_ = pixelHeight_ = 0;

    if (!world_) return;

    textures_ = world_->AllTextures();
    if (!filter_.empty())
        textures_.erase(std::remove_if(textures_.begin(), textures_.end(),
                                       [this](const nfs::Texture* texture) {
                                           return !std::binary_search(filter_.begin(), filter_.end(),
                                                                      texture->hash);
                                       }),
                        textures_.end());

    // The chosen tab keeps one city's textures (tab 0: all of them).
    if (const int city = tabs_ ? TabCtrl_GetCurSel(tabs_) : 0; city > 0)
        textures_.erase(std::remove_if(textures_.begin(), textures_.end(),
                                       [city](const nfs::Texture* texture) { return CityOf(texture->name) != city; }),
                        textures_.end());

    // The search field keeps the textures whose name holds what was typed,
    // whatever the case. ponytail: the list is made again on every key;
    // a short timer if that gets slow on a whole region.
    wchar_t typed[128] = {};
    ::GetDlgItemTextW(dialog_, IDC_TEX_SEARCH, typed, 128);
    std::wstring search = typed;
    for (wchar_t& c : search) c = (wchar_t)::towlower(c);
    if (!search.empty())
        textures_.erase(std::remove_if(textures_.begin(), textures_.end(),
                                       [&search](const nfs::Texture* texture) {
                                           std::wstring name = Widen(texture->name);
                                           for (wchar_t& c : name) c = (wchar_t)::towlower(c);
                                           return name.find(search) == std::wstring::npos;
                                       }),
                        textures_.end());

    // Most used first: those are the ones that decide how the world looks.
    std::sort(textures_.begin(), textures_.end(),
              [this](const nfs::Texture* a, const nfs::Texture* b) {
                  const u32 ua = world_->TextureUsage(a->hash);
                  const u32 ub = world_->TextureUsage(b->hash);
                  if (ua != ub) return ua > ub;
                  return a->name < b->name;
              });

    ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < textures_.size(); ++i) {
        const nfs::Texture& texture = *textures_[i];

        std::wstring name = Widen(texture.name);
        LVITEMW item{};
        item.mask     = LVIF_TEXT | LVIF_PARAM;
        item.iItem    = (int)i;
        item.pszText  = name.data();
        item.lParam   = (LPARAM)i;
        const int row = ListView_InsertItem(list_, &item);

        std::wstring size = Format(L"%ux%u", texture.width, texture.height);
        ListView_SetItemText(list_, row, 1, size.data());

        std::wstring format = Widen(nfs::ToString(texture.format));
        ListView_SetItemText(list_, row, 2, format.data());

        std::wstring used = Format(L"%u", world_->TextureUsage(texture.hash));
        ListView_SetItemText(list_, row, 3, used.data());
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);

    std::wstring title = Format(filter_.empty() ? L"Texture browser - %zu textures"
                                                : L"Texture browser - %zu textures of the selection",
                                textures_.size());
    if (!search.empty()) title += L" named *" + std::wstring(typed) + L"*";
    ::SetWindowTextW(dialog_, title.c_str());

    if (!textures_.empty()) {
        int row = 0;
        for (size_t i = 0; i < textures_.size(); ++i)
            if (chosen && textures_[i]->hash == chosen) { row = (int)i; break; }
        ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, row, FALSE);
    } else {
        OnSelectionChanged();   // nothing listed: the last texture's information goes too
    }
}

const nfs::Texture* TextureBrowser::Selected() const {
    if (!list_) return nullptr;
    const int row = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (row < 0 || (size_t)row >= textures_.size()) return nullptr;
    return textures_[(size_t)row];
}

void TextureBrowser::OnSelectionChanged() {
    const nfs::Texture* texture = Selected();
    pixels_.clear();
    pixelWidth_ = pixelHeight_ = 0;

    // The largest stored version is what is shown and saved: in NFS World the
    // pack holds a small copy and the full size is kept apart.
    shown_ = texture ? *texture : nfs::Texture();
    if (texture && world_) world_->LargestTexture(texture->hash, shown_);
    const nfs::Texture* shown = texture ? &shown_ : nullptr;

    if (shown && !nfs::DecodeTexture(*shown, 0, pixels_, pixelWidth_, pixelHeight_)) {
        pixels_.clear();
        pixelWidth_ = pixelHeight_ = 0;
    }

    if (texture) {
        ::SetDlgItemTextW(dialog_, IDC_TEX_INFO,
                          Format(L"%s\n%u x %u   %s   %u mip levels\nhash %08X   "
                                 L"used by %u materials",
                                 Widen(texture->name).c_str(),
                                 shown->width, shown->height,
                                 Widen(nfs::ToString(texture->format)).c_str(),
                                 shown->mipCount, texture->hash,
                                 world_ ? world_->TextureUsage(texture->hash) : 0).c_str());
    } else {
        ::SetDlgItemTextW(dialog_, IDC_TEX_INFO, L"");
    }

    ::InvalidateRect(::GetDlgItem(dialog_, IDC_TEX_PREVIEW), nullptr, TRUE);
}

void TextureBrowser::DrawPreview(const DRAWITEMSTRUCT& item) {
    const RECT& box = item.rcItem;
    ::FillRect(item.hDC, &box, (HBRUSH)::GetStockObject(DKGRAY_BRUSH));

    if (pixels_.empty()) {
        ::SetBkMode(item.hDC, TRANSPARENT);
        ::SetTextColor(item.hDC, RGB(220, 220, 220));
        RECT text = box;
        ::DrawTextW(item.hDC, L"no preview", -1, &text,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const int boxWidth  = box.right - box.left;
    const int boxHeight = box.bottom - box.top;

    // Integer zoom keeps texels honest: a blurred preview would hide exactly the
    // kind of detail this window exists to judge.
    int drawWidth  = (int)pixelWidth_;
    int drawHeight = (int)pixelHeight_;
    const bool actualSize = ::IsDlgButtonChecked(dialog_, IDC_TEX_ZOOM) == BST_CHECKED;

    if (!actualSize) {
        if (drawWidth > boxWidth || drawHeight > boxHeight) {
            const int divisor = std::max(1, std::max((drawWidth + boxWidth - 1) / boxWidth,
                                                     (drawHeight + boxHeight - 1) / boxHeight));
            drawWidth  /= divisor;
            drawHeight /= divisor;
        } else {
            const int factor = std::max(1, std::min(boxWidth / drawWidth,
                                                    boxHeight / drawHeight));
            drawWidth  *= factor;
            drawHeight *= factor;
        }
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth       = (LONG)pixelWidth_;
    info.bmiHeader.biHeight      = -(LONG)pixelHeight_;   // negative: rows run top-down
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    ::SetStretchBltMode(item.hDC, COLORONCOLOR);
    ::StretchDIBits(item.hDC,
                    box.left + (boxWidth - drawWidth) / 2,
                    box.top + (boxHeight - drawHeight) / 2,
                    drawWidth, drawHeight,
                    0, 0, (int)pixelWidth_, (int)pixelHeight_,
                    pixels_.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}

namespace {

// A PNG of the texture's largest level, alpha included. Written through
// Windows' own imaging component, which the editor already reads images with,
// so no compressor has to be brought in for it.
bool WritePng(const std::wstring& path, const std::vector<u32>& bgra, u32 width, u32 height) {
    if (!width || !height || bgra.size() < (size_t)width * height) return false;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool ok =
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(width, height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) &&
        ::IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(frame->WritePixels(height, width * 4, (UINT)((size_t)width * height * 4), (BYTE*)bgra.data())) &&
        SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit());

    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    factory->Release();
    if (!ok) ::DeleteFileW(path.c_str());
    return ok;
}

} // namespace

void TextureBrowser::SaveSelected() {
    const nfs::Texture* texture = Selected();
    if (!texture) return;

    wchar_t path[MAX_PATH] = {};
    const std::wstring name = Widen(texture->name);
    ::wcsncpy_s(path, name.c_str(), _TRUNCATE);
    ::wcsncat_s(path, L".png", _TRUNCATE);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = dialog_;
    dialog.lpstrFilter = L"PNG image\0*.png\0";
    dialog.lpstrFile   = path;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrDefExt = L"png";
    dialog.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    // FRS_TEST_SAVE=<folder> saves there without asking, for scripted checks.
    wchar_t scripted[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"FRS_TEST_SAVE", scripted, MAX_PATH)) {
        const std::wstring target = std::wstring(scripted) + L"\\" + name + L".png";
        ::wcsncpy_s(path, target.c_str(), _TRUNCATE);
    } else if (!::GetSaveFileNameW(&dialog)) {
        return;
    }

    // Alpha that is not opacity -- roads and walls keep gloss there -- makes the
    // picture look empty in any viewer, though the browser shows it whole. Such
    // a texture is saved opaque; replacing it keeps the alpha the game has.
    std::vector<u32> saved = pixels_;
    if (!nfs::AlphaIsOpacity(*texture))
        for (u32& pixel : saved) pixel |= 0xFF000000u;

    if (saved.empty() || !WritePng(path, saved, pixelWidth_, pixelHeight_))
        ::MessageBoxW(dialog_, L"Could not write the file.", L"Texture browser",
                      MB_ICONERROR | MB_OK);
}

void TextureBrowser::Layout() {
    if (!dialog_ || !list_) return;

    RECT client{};
    ::GetClientRect(dialog_, &client);

    // Everything is measured from the dialog's own font and DPI: sizes in
    // fixed pixels were right at 100% only, and at 150% the three lines of
    // information ran under the buttons.
    const auto dpi = [this](int at96) { return ::MulDiv(at96, (int)::GetDpiForWindow(dialog_), 96); };
    // The height of a line as the information control itself draws it.
    int line = dpi(16);
    HWND info = ::GetDlgItem(dialog_, IDC_TEX_INFO);
    if (HDC dc = ::GetDC(info)) {
        HFONT font = (HFONT)::SendMessageW(info, WM_GETFONT, 0, 0);
        HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
        RECT three{ 0, 0, 1000, 0 };
        if (::DrawTextW(dc, L"Ag\nAg\nAg", -1, &three, DT_CALCRECT) && three.bottom > 0)
            line = (three.bottom + 2) / 3;
        if (previous) ::SelectObject(dc, previous);
        ::ReleaseDC(info, dc);
    }

    const int pad       = dpi(8);
    const int listWidth = std::min(dpi(330), std::max(dpi(180), (int)client.right * 2 / 5));
    const int button    = line + dpi(10);
    const int infoH     = line * 3 + dpi(2);
    // Under the picture: the information, then a row of what changes the
    // texture, then a row of what looks at it and saves it.
    const int footer    = dpi(4) + infoH + pad + button + dpi(6) + button;

    const int searchH = line + dpi(6);
    const int tabsH   = tabs_ ? line + dpi(10) : 0;   // the strip of tabs only: the list is not inside it
    if (tabs_) ::MoveWindow(tabs_, pad, pad, listWidth, tabsH, TRUE);
    const int top = pad + tabsH + (tabs_ ? dpi(4) : 0);
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_SEARCH), pad, top, listWidth, searchH, TRUE);
    ::MoveWindow(list_, pad, top + searchH + dpi(4), listWidth, client.bottom - top - pad - searchH - dpi(4), TRUE);

    const int previewX = pad * 2 + listWidth;
    const int previewW = std::max(dpi(60), (int)client.right - previewX - pad);
    const int previewH = std::max(dpi(60), (int)client.bottom - footer - 2 * pad);
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_PREVIEW), previewX, pad, previewW, previewH, TRUE);

    int y = pad + previewH + dpi(4);
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_INFO), previewX, y, previewW, infoH, TRUE);
    y += infoH + pad;

    const int half = (previewW - dpi(6)) / 2;
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_REPLACE), previewX, y, half, button, TRUE);
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_RESTORE), previewX + half + dpi(6), y, previewW - half - dpi(6),
                 button, TRUE);
    y += button + dpi(6);

    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_ZOOM), previewX, y, half, button, TRUE);
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_SAVE), previewX + half + dpi(6), y, previewW - half - dpi(6),
                 button, TRUE);

    // The whole dialog, children included: a control that only moved is not
    // repainted where it was, which left pieces of the old layout on screen.
    ::RedrawWindow(dialog_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
