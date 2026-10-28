#pragma once

// Virtual file system: guest device paths (\Device\Cdrom0\...) mapped onto host storage, plus the
// kernel's symbolic links (\??\D: -> \Device\Cdrom0). Paths are case-insensitive, as on the 360.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vfs
{
    struct EntryInfo
    {
        std::string name;          // file name only
        bool isDirectory = false;
        uint64_t size = 0;
        uint64_t allocationSize = 0;
        uint64_t creationTime = 0; // FILETIME (100 ns since 1601)
        uint64_t accessTime = 0;
        uint64_t writeTime = 0;
        uint32_t attributes = 0;   // FILE_ATTRIBUTE_*
    };

    inline constexpr uint32_t kAttributeReadOnly = 0x01;
    inline constexpr uint32_t kAttributeDirectory = 0x10;
    inline constexpr uint32_t kAttributeNormal = 0x80;

    // An open file or directory on a device. Reads and writes are positional and thread-safe.
    class File
    {
    public:
        virtual ~File() = default;
        virtual const EntryInfo& Info() const = 0;
        // Return NTSTATUS.
        virtual uint32_t Read(void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) = 0;
        virtual uint32_t Write(const void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) = 0;
        virtual uint32_t SetSize(uint64_t size) = 0;
        virtual uint32_t Flush() { return 0; }
        // Directory contents (directories only).
        virtual std::vector<EntryInfo> List() const { return {}; }
        // Marks the file to be deleted when closed.
        virtual uint32_t SetDeleteOnClose() = 0;
    };

    // CreateDisposition values.
    enum class Disposition : uint32_t
    {
        Supersede = 0,
        Open = 1,
        Create = 2,
        OpenIf = 3,
        Overwrite = 4,
        OverwriteIf = 5,
    };

    // IO_STATUS_BLOCK.Information values for opens.
    enum class OpenResult : uint32_t
    {
        Superseded = 0,
        Opened = 1,
        Created = 2,
        Overwritten = 3,
    };

    struct OpenRequest
    {
        std::string path; // relative to the device root, '\'-separated, no leading slash
        Disposition disposition = Disposition::Open;
        bool directory = false;    // FILE_DIRECTORY_FILE
        bool nonDirectory = false; // FILE_NON_DIRECTORY_FILE
        bool write = false;
    };

    class Device
    {
    public:
        virtual ~Device() = default;
        virtual const char* Name() const = 0;
        virtual bool ReadOnly() const = 0;
        // Returns NTSTATUS; on success sets `file` and `result`.
        virtual uint32_t Open(const OpenRequest& request, std::unique_ptr<File>& file, OpenResult& result) = 0;
        virtual uint32_t Query(const std::string& path, EntryInfo& info) = 0;
        virtual uint64_t TotalBytes() const = 0;
        virtual uint64_t FreeBytes() const = 0;
        virtual const char* FileSystemName() const = 0;
    };

    // Mounts a device at an absolute guest path such as "\Device\Cdrom0".
    void Mount(std::string_view mountPoint, std::unique_ptr<Device> device);
    void Unmount(std::string_view mountPoint);

    // Symbolic links (e.g. "\??\D:" -> "\Device\Cdrom0"). Return false if the link doesn't exist / already exists.
    bool AddSymbolicLink(std::string_view link, std::string_view target);
    bool RemoveSymbolicLink(std::string_view link);

    // Resolves a guest path through symbolic links to a device and a device-relative path.
    // Returns nullptr if no device covers the path.
    Device* Resolve(std::string_view guestPath, std::string& relativePath);

    // Joins a directory path and a relative name with a single backslash.
    std::string JoinPath(std::string_view directory, std::string_view name);
}
