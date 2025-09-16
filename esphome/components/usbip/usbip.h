#pragma once

#include "esphome/core/component.h"
#include <string>
#include <memory>
#include "usb_host.h"
#include <vector>
#include <unordered_map>

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
  // How long (ms) to wait for string descriptor fetches when responding to
  // an OP_REQ_DEVLIST. Exposed so codegen can set from YAML.
  void set_string_wait_ms(uint32_t ms) { string_wait_ms_ = ms; }

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

  // How long (ms) to wait for string descriptor fetches when responding to
  // an OP_REQ_DEVLIST before finishing the reply. Small values reduce
  // latency but may result in missing human-readable names in the first
  // response.

  // State for non-blocking OP_REQ_DEVLIST handling: when an OP_REQ_DEVLIST is
  // received we request descriptors asynchronously and finish the reply in
  // subsequent loop() calls when descriptors are ready or timeout expires.
  bool pending_devlist_{false};
  // Millis deadline when pending devlist should be completed regardless
  uint32_t pending_devlist_deadline_{0};

  // How long to wait for string descriptors during a pending devlist
  // operation (see set_string_wait_ms()).
  uint32_t string_wait_ms_{2000};

  // Non-blocking send buffer/state used to stream OP_REP_DEVLIST replies
  // across multiple loop() iterations so we never block the main loop.
  std::vector<uint8_t> send_buf_{};
  size_t send_offset_{0};
  bool sending_devlist_{false};

  // Per-client map of last time (ms) we attempted to request a string
  // descriptor for a given index. This avoids hammering the USB host.
  std::vector<std::unordered_map<int, uint32_t>> last_string_request_ms_{};
  // Minimum ms between retry attempts for the same string index
  uint32_t string_request_interval_ms_{200};
};

}  // namespace usbip
}  // namespace esphome
