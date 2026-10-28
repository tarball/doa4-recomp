// Writable device backed by a host directory (hard disk partitions: saves, cache).
#include "vfs/iso_device.h"

#include "kernel/status.h"
#include "log.h"

#include <filesystem>
#include <system_error>

#include <windows.h>

namespace vfs
{
    namespace
    {
        namespace fs = std::filesystem;

        uint64_t ToFileTime(const FILETIME& time)
        {
            return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        }

        uint32_t StatusFromWin32(DWORD error)
        {
            switch (error)
            {
            case ERROR_FILE_NOT_FOUND: return status::kObjectNameNotFound;
            case ERROR_PATH_NOT_FOUND: return status::kObjectPathNotFound;
            case ERROR_ACCESS_DENIED: return status::kAccessDenied;
            case ERROR_FILE_EXISTS:
            case ERROR_ALREADY_EXISTS: return 0xC0000035; // STATUS_OBJECT_NAME_COLLISION
            case ERROR_DISK_FULL: return 0xC000007F;      // STATUS_DISK_FULL
            case ERROR_DIR_NOT_EMPTY: return 0xC0000101;  // STATUS_DIRECTORY_NOT_EMPTY
            default: return status::kUnsuccessful;
            }
        }

        bool FillInfo(const fs::path& path, EntryInfo& info)
        {
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                return false;
            info.name = path.filename().string();
            info.isDirectory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            info.size = info.isDirectory ? 0 : (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            info.allocationSize = (info.size + 0xFFF) & ~uint64_t(0xFFF);
            info.creationTime = ToFileTime(data.ftCreationTime);
            info.accessTime = ToFileTime(data.ftLastAccessTime);
            info.writeTime = ToFileTime(data.ftLastWriteTime);
            info.attributes = info.isDirectory ? kAttributeDirectory : kAttributeNormal;
            return true;
        }

        class HostFile : public File
        {
        public:
            HostFile(fs::path path, HANDLE handle, EntryInfo info) : m_path(std::move(path)), m_handle(handle), m_info(std::move(info)) {}

            ~HostFile() override
            {
                if (m_handle != INVALID_HANDLE_VALUE)
                    CloseHandle(m_handle);
                if (m_deleteOnClose)
                {
                    std::error_code error;
                    fs::remove(m_path, error);
                }
            }

            const EntryInfo& Info() const override
            {
                FillInfo(m_path, m_info); // sizes and times change as the file is written
                return m_info;
            }

            uint32_t Read(void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) override
            {
                transferred = 0;
                if (m_info.isDirectory)
                    return status::kInvalidParameter;
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD read = 0;
                if (!ReadFile(m_handle, buffer, length, &read, &overlapped))
                    return GetLastError() == ERROR_HANDLE_EOF ? status::kEndOfFile : StatusFromWin32(GetLastError());
                transferred = read;
                return (read == 0 && length != 0) ? status::kEndOfFile : status::kSuccess;
            }

            uint32_t Write(const void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) override
            {
                transferred = 0;
                if (m_info.isDirectory)
                    return status::kInvalidParameter;
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD written = 0;
                if (!WriteFile(m_handle, buffer, length, &written, &overlapped))
                    return StatusFromWin32(GetLastError());
                transferred = written;
                return status::kSuccess;
            }

            uint32_t SetSize(uint64_t size) override
            {
                FILE_END_OF_FILE_INFO info{};
                info.EndOfFile.QuadPart = LONGLONG(size);
                return SetFileInformationByHandle(m_handle, FileEndOfFileInfo, &info, sizeof(info)) ? status::kSuccess : StatusFromWin32(GetLastError());
            }

            uint32_t Flush() override
            {
                return (m_info.isDirectory || FlushFileBuffers(m_handle)) ? status::kSuccess : StatusFromWin32(GetLastError());
            }

            std::vector<EntryInfo> List() const override
            {
                std::vector<EntryInfo> entries;
                std::error_code error;
                for (const auto& entry : fs::directory_iterator(m_path, error))
                {
                    EntryInfo info;
                    if (FillInfo(entry.path(), info))
                        entries.push_back(std::move(info));
                }
                return entries;
            }

            uint32_t SetDeleteOnClose() override
            {
                m_deleteOnClose = true;
                return status::kSuccess;
            }

        private:
            fs::path m_path;
            HANDLE m_handle;
            mutable EntryInfo m_info;
            bool m_deleteOnClose = false;
        };

        class HostDevice : public Device
        {
        public:
            HostDevice(std::string name, fs::path root) : m_name(std::move(name)), m_root(std::move(root)) {}

            const char* Name() const override { return m_name.c_str(); }
            bool ReadOnly() const override { return false; }

            uint32_t Open(const OpenRequest& request, std::unique_ptr<File>& file, OpenResult& result) override
            {
                fs::path path;
                if (!HostPath(request.path, path))
                    return status::kObjectPathNotFound;

                std::error_code error;
                const bool exists = fs::exists(path, error);
                const bool isDirectory = exists && fs::is_directory(path, error);

                if (request.directory || isDirectory)
                {
                    if (request.nonDirectory && isDirectory)
                        return 0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY
                    if (!exists)
                    {
                        if (request.disposition != Disposition::Create && request.disposition != Disposition::OpenIf)
                            return fs::exists(path.parent_path(), error) ? status::kObjectNameNotFound : status::kObjectPathNotFound;
                        if (!fs::create_directory(path, error))
                            return fs::exists(path.parent_path()) ? status::kUnsuccessful : status::kObjectPathNotFound;
                        result = OpenResult::Created;
                    }
                    else
                    {
                        if (request.disposition == Disposition::Create)
                            return 0xC0000035; // STATUS_OBJECT_NAME_COLLISION
                        if (!isDirectory)
                            return 0xC0000103; // STATUS_NOT_A_DIRECTORY
                        result = OpenResult::Opened;
                    }
                    EntryInfo info;
                    FillInfo(path, info);
                    file = std::make_unique<HostFile>(path, INVALID_HANDLE_VALUE, info);
                    return status::kSuccess;
                }

                DWORD creation;
                switch (request.disposition)
                {
                case Disposition::Supersede: creation = CREATE_ALWAYS; result = exists ? OpenResult::Superseded : OpenResult::Created; break;
                case Disposition::Open: creation = OPEN_EXISTING; result = OpenResult::Opened; break;
                case Disposition::Create: creation = CREATE_NEW; result = OpenResult::Created; break;
                case Disposition::OpenIf: creation = OPEN_ALWAYS; result = exists ? OpenResult::Opened : OpenResult::Created; break;
                case Disposition::Overwrite: creation = TRUNCATE_EXISTING; result = OpenResult::Overwritten; break;
                case Disposition::OverwriteIf: creation = CREATE_ALWAYS; result = exists ? OpenResult::Overwritten : OpenResult::Created; break;
                default: return status::kInvalidParameter;
                }

                const bool write = request.write || creation != OPEN_EXISTING;
                HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                    return StatusFromWin32(GetLastError());

                EntryInfo info;
                FillInfo(path, info);
                file = std::make_unique<HostFile>(path, handle, info);
                return status::kSuccess;
            }

            uint32_t Query(const std::string& relative, EntryInfo& info) override
            {
                fs::path path;
                if (!HostPath(relative, path))
                    return status::kObjectPathNotFound;
                if (!FillInfo(path, info))
                {
                    std::error_code error;
                    return fs::exists(path.parent_path(), error) ? status::kObjectNameNotFound : status::kObjectPathNotFound;
                }
                return status::kSuccess;
            }

            uint64_t TotalBytes() const override { return 20ull * 1024 * 1024 * 1024; } // a 20 GB retail drive
            uint64_t FreeBytes() const override
            {
                std::error_code error;
                const auto space = fs::space(m_root, error);
                return error ? 0 : std::min<uint64_t>(space.available, TotalBytes());
            }
            const char* FileSystemName() const override { return "FATX"; }

        private:
            // Maps a guest relative path into the root, refusing anything that escapes it.
            bool HostPath(const std::string& relative, fs::path& out) const
            {
                fs::path path = m_root;
                size_t start = 0;
                while (start <= relative.size())
                {
                    const size_t end = std::min(relative.find('\\', start), relative.size());
                    const std::string part = relative.substr(start, end - start);
                    if (part == "..")
                        return false;
                    if (!part.empty() && part != ".")
                        path /= part;
                    start = end + 1;
                }
                out = path;
                return true;
            }

            std::string m_name;
            fs::path m_root;
        };
    }

    std::unique_ptr<Device> OpenHostDevice(const std::string& name, const std::string& hostDirectory)
    {
        std::error_code error;
        fs::create_directories(hostDirectory, error);
        if (error)
        {
            LOG_ERROR("Can't create %s: %s", hostDirectory.c_str(), error.message().c_str());
            return nullptr;
        }
        return std::make_unique<HostDevice>(name, fs::absolute(hostDirectory));
    }
}
