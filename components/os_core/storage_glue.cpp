// Glue between svc_storage and the framework (Fs for apps, Topic::Storage events).
#include "core_internal.h"

namespace os::core {

Fs* fsService() { return nullptr; }
void storageStart() {}

}  // namespace os::core
