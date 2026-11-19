#pragma once

#include "core/Types.h"

#include <string>

// A read-only memory mapping of a file on disk.
//
// Track streams reach 120 MB each and the editor keeps several open at once, so
// mapping beats reading into heap buffers: the parsers walk the chunk tree in
// place and only the pages they actually touch are paged in.
class FileMap {
public:
    FileMap() = default;
    ~FileMap();

    FileMap(const FileMap&)            = delete;
    FileMap& operator=(const FileMap&) = delete;

    FileMap(FileMap&& other) noexcept;
    FileMap& operator=(FileMap&& other) noexcept;

    // Maps the whole file. Returns false and logs the reason on failure.
    bool Open(const std::wstring& path);
    void Close();

    // NFS World ships each streaming section as its own file. The editor works
    // on one stream addressed by offset, so those files are laid end to end in
    // a reserved address range and read into it as each section is opened:
    // Reserve claims the range, Load fills one file's part of it.
    bool Reserve(u64 size);
    bool Load(u64 offset, const std::wstring& path, u64 size);

    bool IsOpen() const { return base_ != nullptr; }

    ByteSpan Span() const { return { base_, size_ }; }

    const std::wstring& Path() const { return path_; }
    u64                 Size() const { return size_; }

private:
    std::wstring path_;
    void*        file_    = nullptr;  // HANDLE
    void*        mapping_ = nullptr;  // HANDLE
    const u8*    base_    = nullptr;
    u64          size_    = 0;
    bool         reserved_ = false;   // base_ is a VirtualAlloc range, not a view
};
