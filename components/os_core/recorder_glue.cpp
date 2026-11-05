// Glue between the microphone recorder service and the framework.
#include "core_internal.h"

namespace os::core {

Recorder* recorderService() { return nullptr; }
void recorderStart() {}

}  // namespace os::core
