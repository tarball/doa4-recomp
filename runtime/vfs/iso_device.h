#pragma once

#include "vfs/vfs.h"

namespace vfs
{
    // Read-only XDVDFS (Xbox game disc) file system read directly from a disc image.
    std::unique_ptr<Device> OpenIsoDevice(const std::string& isoPath);

    // A writable device backed by a host directory (created if missing).
    std::unique_ptr<Device> OpenHostDevice(const std::string& name, const std::string& hostDirectory);
}
