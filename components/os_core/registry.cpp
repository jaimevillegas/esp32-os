#include "core_internal.h"
#include "sdkconfig.h"

// apps.def lists the apps enabled in Kconfig as OS_APP(name, namespace).
#define OS_APP(name, ns) namespace ns { extern const os::Manifest kManifest; }
#include "apps.def"
#undef OS_APP

namespace os::core {

#define OS_APP(name, ns) &ns::kManifest,
static const Manifest* const kApps[] = {
#include "apps.def"
    nullptr,   // keeps the array non-empty when every app is disabled
};
#undef OS_APP

size_t appCount() { return sizeof kApps / sizeof kApps[0] - 1; }
const Manifest& appManifest(size_t idx) { return *kApps[idx]; }

}  // namespace os::core
