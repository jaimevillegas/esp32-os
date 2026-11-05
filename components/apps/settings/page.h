#pragma once
// A page of the Settings app. Each page lives in its own file and is owned by one feature.
#include <memory_resource>

#include "os/app.h"

namespace apps::settings {

class Page {
 public:
  virtual ~Page() = default;
  virtual const char* title() const = 0;
  // Build the page under `body` (the area below the page header, about 456x224). `root` is the
  // whole app area (456x264): use it as the parent of full-area overlays such as a keyboard.
  virtual void build(lv_obj_t* body, lv_obj_t* root, os::Context& ctx) = 0;
  virtual void onEvent(const os::Msg&) {}      // forwarded while the page is visible
  virtual void teardown() {}                    // delete timers / overlays; body is deleted after
};

// Factories construct the page inside the app arena.
Page* makeDisplayPage(std::pmr::memory_resource& m);
Page* makeWifiPage(std::pmr::memory_resource& m);
Page* makeBluetoothPage(std::pmr::memory_resource& m);

}  // namespace apps::settings
