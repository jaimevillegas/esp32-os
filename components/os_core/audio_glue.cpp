// Glue between svc_audio and the framework (Audio for apps, Topic::Audio events).
#include "core_internal.h"

namespace os::core {

Audio* audioService() { return nullptr; }
void audioStart() {}

}  // namespace os::core
