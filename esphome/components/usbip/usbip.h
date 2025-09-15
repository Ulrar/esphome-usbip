#pragma once

#include "esphome/core/component.h"
#include <string>

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

 protected:
  // The TCP port to listen on for USB/IP connections
  uint16_t port_{3240};
};

}  // namespace usbip
}  // namespace esphome
