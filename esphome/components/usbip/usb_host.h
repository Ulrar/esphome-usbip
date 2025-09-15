#pragma once

#include "esphome/core/log.h"
#include <memory>
#include <vector>
#include <cstdint>

// Forward declarations for esphome usb_host types (placed at top-level so
// they are available to adapters without nesting issues).
namespace esphome {
namespace usb_host {
class USBHost;
class USBClient;
}  // namespace usb_host
}  // namespace esphome

namespace esphome {
namespace usbip {

static const char *USB_HOST_TAG = "usbip.host";

// Abstract USB host adapter interface. Implement this for a real USB host
// backend (ESP-IDF, TinyUSB, etc.). The dummy implementation provided in
// usb_host.cpp is only for scaffolding and testing.
class USBHostAdapter {
 public:
  virtual ~USBHostAdapter() = default;

  // Initialize the host stack. Return true on success.
  virtual bool begin() = 0;

  // Stop and clean up the host stack.
  virtual void stop() = 0;

  // Poll the host stack; should be cheap and non-blocking.
  virtual void poll() = 0;

  // Request a device descriptor for the given client (client pointer used
  // as an opaque handle). This is asynchronous; implementations should
  // cache the descriptor once retrieved.
  virtual void request_device_descriptor(void *client_ptr) = 0;

  // Retrieve a cached device descriptor for a client. Returns true if a
  // cached descriptor is available and copied into 'out'.
  virtual bool get_device_descriptor(void *client_ptr, std::vector<uint8_t> &out) = 0;

  // TODO: add methods to enumerate devices and submit transfers.
};

// Factory to create a simple dummy host implementation (no real USB access).
std::unique_ptr<USBHostAdapter> make_dummy_usb_host();

#ifdef ESP_PLATFORM
std::unique_ptr<USBHostAdapter> make_esp_idf_usb_host();
#endif

std::unique_ptr<USBHostAdapter> make_esphome_usb_host_adapter(esphome::usb_host::USBHost *host);

}  // namespace usbip
}  // namespace esphome
