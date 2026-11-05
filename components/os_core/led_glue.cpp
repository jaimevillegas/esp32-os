// Glue between the RGB LED service and the framework.
#include "core_internal.h"

namespace os::core {

Led* ledService() { return nullptr; }
void ledStart() {}

}  // namespace os::core
