#include "core/FileMap.h"

#include "core/Log.h"

#include <windows.h>

#include <algorithm>
#include <utility>

namespace {

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                             nullptr, 0, nullptr, nullptr);
    std::string out((size_t)needed, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace

FileMap::~FileMap() {
    Close();
}

FileMap::FileMap(FileMap&& other) noexcept {
    *this = std::move(other);
}

FileMap& FileMap::operator=(FileMap&& other) noexcept {
    if (this != &other) {
        Close();
        path_    = std::move(other.path_);
        file_    = std::exchange(other.file_, nullptr);
        mapping_ = std::exchange(other.mapping_, nullptr);
        base_    = std::exchange(other.base_, nullptr);
        size_    = std::exchange(other.size_, 0);
        reserved_ = std::exchange(other.reserved_, false);
    }
    return *this;
}

bool FileMap::Open(const std::wstring& path) {
    Close();

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LOG_ERROR("cannot open %s (win32 error %lu)", Narrow(path).c_str(), ::GetLastError());
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!::GetFileSizeEx(file, &fileSize) || fileSize.QuadPart == 0) {
        LOG_ERROR("cannot size %s", Narrow(path).c_str());
        ::CloseHandle(file);
        return false;
    }

    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        LOG_ERROR("cannot map %s (win32 error %lu)", Narrow(path).c_str(), ::GetLastError());
        ::CloseHandle(file);
        return false;
    }

    const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        LOG_ERROR("cannot view %s (win32 error %lu)", Narrow(path).c_str(), ::GetLastError());
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        return false;
    }

    path_    = path;
    file_    = file;
    mapping_ = mapping;
    base_    = static_cast<const u8*>(view);
    size_    = (u64)fileSize.QuadPart;
    return true;
}

bool FileMap::Reserve(u64 size) {
    Close();
    void* range = ::VirtualAlloc(nullptr, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
    if (!range) {
        LOG_ERROR("cannot reserve %llu bytes for the stream (win32 error %lu)",
                  (unsigned long long)size, ::GetLastError());
        return false;
    }
    base_     = static_cast<const u8*>(range);
    size_     = size;
    reserved_ = true;
    return true;
}

bool FileMap::Load(u64 offset, const std::wstring& path, u64 size) {
    if (!reserved_ || offset + size > size_) return false;
    if (size == 0) return true;

    u8* target = const_cast<u8*>(base_) + offset;
    if (!::VirtualAlloc(target, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE)) {
        LOG_ERROR("cannot commit %llu bytes for %s (win32 error %lu)",
                  (unsigned long long)size, Narrow(path).c_str(), ::GetLastError());
        return false;
    }

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LOG_ERROR("cannot open %s (win32 error %lu)", Narrow(path).c_str(), ::GetLastError());
        return false;
    }

    u64 done = 0;
    while (done < size) {
        DWORD read = 0;
        const DWORD want = (DWORD)std::min<u64>(size - done, 1u << 24);
        if (!::ReadFile(file, target + done, want, &read, nullptr) || read == 0) break;
        done += read;
    }
    ::CloseHandle(file);

    if (done != size)
        LOG_WARN("%s: read %llu of %llu bytes", Narrow(path).c_str(),
                 (unsigned long long)done, (unsigned long long)size);
    return done == size;
}

void FileMap::Close() {
    if (reserved_) {
        if (base_) ::VirtualFree(const_cast<u8*>(base_), 0, MEM_RELEASE);
        base_ = nullptr;
        reserved_ = false;
    }
    if (base_)    ::UnmapViewOfFile(base_);
    if (mapping_) ::CloseHandle((HANDLE)mapping_);
    if (file_)    ::CloseHandle((HANDLE)file_);

    path_.clear();
    file_    = nullptr;
    mapping_ = nullptr;
    base_    = nullptr;
    size_    = 0;
}
