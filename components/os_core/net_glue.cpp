// Glue between the Wi-Fi service and the framework.
#include "core_internal.h"

namespace os::core {

Net* netService() { return nullptr; }
void netStart() {}

}  // namespace os::core
