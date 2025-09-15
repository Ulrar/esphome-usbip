#include "usbip.h"
#include "esphome/core/log.h"

namespace esphome {
namespace usbip {

static const char *TAG = "usbip";

void USBIPComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up USB/IP server (port=%u)", this->port_);
  // TODO: initialize USB/IP server stack and start listening on port_
}

void USBIPComponent::loop() {
  // TODO: poll or maintain connection
}

void USBIPComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "USB/IP server:");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
}

}  // namespace usbip
}  // namespace esphome
