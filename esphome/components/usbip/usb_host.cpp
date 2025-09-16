#include "usb_host.h"
#include <chrono>
#include <thread>

namespace esphome {
namespace usbip {

class DummyUSBHost : public USBHostAdapter {
 public:
  bool begin() override {
    ESP_LOGI(USB_HOST_TAG, "Dummy USB host started");
    return true;
  }

  void stop() override { ESP_LOGI(USB_HOST_TAG, "Dummy USB host stopped"); }

  void poll() override {
    // Nothing to do; in a real host this would process events
  }

  void request_device_descriptor(void *client_ptr) override {
    (void)client_ptr;
    // Nothing to do for dummy; descriptor will be provided on get
  }

  void request_config_descriptor(void *client_ptr) override {
    (void)client_ptr;
    // Dummy: no-op
  }

  void request_string_descriptor(void *client_ptr, int index) override {
    (void)client_ptr; (void)index;
    // Dummy: no-op
  }

  bool get_device_descriptor(void *client_ptr, std::vector<uint8_t> &out) override {
    (void)client_ptr;
    // Return a minimal fake device descriptor (18 bytes)
    const uint8_t desc[18] = {
        18, // bLength
        0x01, // bDescriptorType = Device
        0x00, 0x02, // bcdUSB 2.00
        0x00, // bDeviceClass
        0x00, // bDeviceSubClass
        0x00, // bDeviceProtocol
        64,   // bMaxPacketSize0
        0x34, 0x12, // idVendor = 0x1234
        0x78, 0x56, // idProduct = 0x5678
        0x00, 0x01, // bcdDevice
        1, // iManufacturer
        2, // iProduct
        3, // iSerialNumber
        1  // bNumConfigurations
    };
    out.assign(desc, desc + sizeof(desc));
    return true;
  }

  bool get_config_descriptor(void *client_ptr, std::vector<uint8_t> &out) override {
    (void)client_ptr;
    out.clear();
    return false;
  }

  bool get_string_descriptor(void *client_ptr, int index, std::vector<uint8_t> &out) override {
    (void)client_ptr; (void)index;
    out.clear();
    return false;
  }
};

std::unique_ptr<USBHostAdapter> make_dummy_usb_host() {
  return std::unique_ptr<USBHostAdapter>(new DummyUSBHost());
}

#ifdef ESP_PLATFORM
// Placeholder for an ESP-IDF backed USB host adapter. This file intentionally
// keeps the implementation lightweight; fill with real ESP-IDF USB host calls
// when ready.
class EspIdfUSBHost : public USBHostAdapter {
 public:
  bool begin() override {
    ESP_LOGI(USB_HOST_TAG, "ESP-IDF USB host adapter initializing (stub)");
    // TODO: initialize actual ESP-IDF USB Host stack here.
    return true;  // return true if initialization succeeds
  }

  void stop() override { ESP_LOGI(USB_HOST_TAG, "ESP-IDF USB host adapter stopped (stub)"); }

  void poll() override {
    // TODO: poll ESP-IDF USB host events, process transfers
  }

  void request_device_descriptor(void *client_ptr) override {
    (void)client_ptr;
    // TODO: implement using ESP-IDF usb_host get descriptor APIs
  }

  void request_config_descriptor(void *client_ptr) override {
    (void)client_ptr;
    // TODO: implement when ESP-IDF adapter is ready
  }

  void request_string_descriptor(void *client_ptr, int index) override {
    (void)client_ptr; (void)index;
    // TODO: implement when ESP-IDF adapter is ready
  }

  bool get_device_descriptor(void *client_ptr, std::vector<uint8_t> &out) override {
    (void)client_ptr;
    return false;
  }

  bool get_config_descriptor(void *client_ptr, std::vector<uint8_t> &out) override {
    (void)client_ptr;
    return false;
  }

  bool get_string_descriptor(void *client_ptr, int index, std::vector<uint8_t> &out) override {
    (void)client_ptr; (void)index;
    return false;
  }
};

std::unique_ptr<USBHostAdapter> make_esp_idf_usb_host() {
  return std::unique_ptr<USBHostAdapter>(new EspIdfUSBHost());
}
#endif


}  // namespace usbip
}  // namespace esphome
