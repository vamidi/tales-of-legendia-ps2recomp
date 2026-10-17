#pragma once

#include "iop_service.h"

#include <functional>
#include <string_view>

namespace ps2x::iop::detail
{
    // Loads an IOP module by path through the subsystem: the physical IRX when
    // the file is on the disc tree, the registered HLE provider otherwise.
    using LoadModuleCallback = std::function<ModuleLoadResult(std::string_view)>;

    std::unique_ptr<IopService> createDbcmanService(IopHost &host);
    std::unique_ptr<IopService> createLibSdService(IopHost &host);
    std::unique_ptr<IopService> createMcservService(IopHost &host);
    std::unique_ptr<IopService> createLoadFileService(IopHost &host, LoadModuleCallback load);
}
