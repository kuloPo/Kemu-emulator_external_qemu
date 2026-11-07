#include "UpdateCore.h"

namespace kemu::update {
Installer fetchInstaller(const std::string& version) {
    validateVersion(version);
    // Keep the trusted response in memory, separate from the writable download cache.
    return selectInstaller(Json::parse(downloadText(std::string(ApiReleasesUrl) + version, 8*1024*1024)), version);
}
}
