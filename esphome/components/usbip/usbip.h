#pragma once

#include "esphome/core/component.h"
#include <string>
#include <memory>
#include "usb_host.h"
#include <vector>

namespace esphome {
namespace usbip {

class USBIPComponent : public Component {
 public:
  USBIPComponent() = default;

  void setup() override;
  void loop() override;
  void dump_config() override;

  // configuration
  void set_port(uint16_t port) { port_ = port; }

  // Inject a USB host adapter (ownership transferred). If not set, the
  // component will not attempt to access USB host functionality.
  void set_host_adapter(std::unique_ptr<USBHostAdapter> host) { host_ = std::move(host); }

  // Directly bind to an esphome usb_host::USBHost instance. This creates an
  // adapter that delegates to the provided host.
  void set_esphome_host(void *host_ptr);
  // Register a USBClient (from esphome::usb_host) to be exported over USB/IP.
  void add_exported_client(void *client_ptr);

 protected:
  // The TCP port to listen on for USB/IP connections
  uint16_t port_{3240};
  // Listening socket file descriptor (or -1 if unused)
  int server_fd_{-1};
  // Accepted client socket file descriptor (or -1 if unused)
  int client_fd_{-1};
  // Whether the TCP server has been started
  bool server_started_{false};

  // Start the TCP server (bind/listen). Called from loop() to defer risky
  // operations until after setup() logs have been emitted.
  void start_server();
  // Optional USB host adapter used to access attached USB devices
  std::unique_ptr<USBHostAdapter> host_{nullptr};
  // Registered USB clients to export
  std::vector<void *> exported_clients_{};
  // Cached device descriptors per exported client (same index as exported_clients_)
  std::vector<std::vector<uint8_t>> client_descriptors_{};
  // Request descriptors for registered clients
  void request_client_descriptors();
  // Try to update cached descriptors (non-blocking)
  void update_client_descriptors();
};

}  // namespace usbip
}  // namespace esphome
