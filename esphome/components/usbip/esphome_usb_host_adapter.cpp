#include "usb_host.h"

#ifdef ESP_PLATFORM
#include "esphome/components/usb_host/usb_host.h"
#include <unordered_map>
#include <vector>

namespace esphome {
namespace usbip {

class EsphomeUsbHostAdapter : public USBHostAdapter {
 public:
  explicit EsphomeUsbHostAdapter(esphome::usb_host::USBHost *host) : host_(host) {}

  bool begin() override {
    ESP_LOGI(USB_HOST_TAG, "Esphome USB host adapter bound to host instance");
    // esphome's USBHost is itself a Component; assume it's already setup by
    // the esphome runtime. We return true since we successfully bound.
    return true;
  }

  void stop() override { ESP_LOGI(USB_HOST_TAG, "Esphome USB host adapter stopped"); }

  void poll() override {
    if (this->host_) {
      // Delegate to the host component loop() which will process events.
      this->host_->loop();
    }
  }

  void request_device_descriptor(void *client_ptr) override {
    if (!client_ptr) return;
    auto client = static_cast<esphome::usb_host::USBClient *>(client_ptr);
    if (!client) return;

    ESP_LOGI(USB_HOST_TAG, "Requesting device descriptor via USBClient control_transfer");

    // Build bmRequestType for Device GET_DESCRIPTOR (IN, standard, device)
    uint8_t bmRequestType = esphome::usb_host::USB_DIR_IN | esphome::usb_host::USB_TYPE_STANDARD |
                            esphome::usb_host::USB_RECIP_DEVICE;
    const uint8_t REQUEST_GET_DESCRIPTOR = 0x06;
    const uint16_t VALUE_DEVICE_DESCRIPTOR = (1 << 8);  // (DT_DEVICE << 8)
    const uint16_t INDEX = 0;
    const uint16_t LENGTH = 18;  // device descriptor length

    // Callback to receive transfer result
    auto cb = [this, client_ptr](const esphome::usb_host::TransferStatus &st) {
      if (st.success && st.data && st.data_len > 0) {
        std::vector<uint8_t> v(st.data, st.data + st.data_len);
        auto &set = this->desc_cache_[client_ptr];
        set.device = v;
        ESP_LOGI(USB_HOST_TAG, "Received %u bytes device descriptor for client", (unsigned)st.data_len);

        // Parse device descriptor for string indices and configuration count
        if (st.data_len >= 18) {
          uint8_t iManufacturer = st.data[14];
          uint8_t iProduct = st.data[15];
          uint8_t iSerial = st.data[16];
          uint8_t bNumConfigurations = st.data[17];

          // Request configuration descriptor (first request to get total length)
          uint8_t bmReqCfg = esphome::usb_host::USB_DIR_IN | esphome::usb_host::USB_TYPE_STANDARD |
                             esphome::usb_host::USB_RECIP_DEVICE;
          const uint8_t REQ_GET_DESCRIPTOR = 0x06;
          const uint16_t VALUE_CFG_DESC = (2 << 8);  // CONFIG descriptor
          const uint16_t INDEX0 = 0;
          // First request: only 9 bytes to read wTotalLength
          auto cfg_cb1 = [this, client_ptr, bmReqCfg](const esphome::usb_host::TransferStatus &st2) {
            if (st2.success && st2.data_len >= 9) {
              uint16_t total_len = st2.data[2] | (st2.data[3] << 8);
              ESP_LOGI(USB_HOST_TAG, "Configuration total length=%u", (unsigned)total_len);
              // Now request the full configuration descriptor
              auto cfg_cb2 = [this, client_ptr](const esphome::usb_host::TransferStatus &st3) {
                if (st3.success && st3.data_len > 0) {
                  auto &set2 = this->desc_cache_[client_ptr];
                  set2.config.assign(st3.data, st3.data + st3.data_len);
                  ESP_LOGI(USB_HOST_TAG, "Cached configuration descriptor (%u bytes)", (unsigned)st3.data_len);
                } else {
                  ESP_LOGW(USB_HOST_TAG, "Full config descriptor fetch failed");
                }
              };
              std::vector<uint8_t> dummy3(total_len);
              auto client = static_cast<esphome::usb_host::USBClient *>(client_ptr);
              client->control_transfer(bmReqCfg, REQ_GET_DESCRIPTOR, VALUE_CFG_DESC, INDEX0, cfg_cb2, dummy3);
            } else {
              ESP_LOGW(USB_HOST_TAG, "Config descriptor probe failed");
            }
          };
          std::vector<uint8_t> probe(9);
          auto client2 = static_cast<esphome::usb_host::USBClient *>(client_ptr);
          client2->control_transfer(bmReqCfg, REQ_GET_DESCRIPTOR, VALUE_CFG_DESC, INDEX0, cfg_cb1, probe);

          // Request string descriptors if indexes are present
          auto request_string = [this, client_ptr](uint8_t idx) {
            if (idx == 0) return;
            uint8_t bmReqStr = esphome::usb_host::USB_DIR_IN | esphome::usb_host::USB_TYPE_STANDARD |
                               esphome::usb_host::USB_RECIP_DEVICE;
            const uint8_t REQ_GET_DESCRIPTOR = 0x06;
            const uint16_t VALUE_STR = (3 << 8) | idx;  // STRING descriptor with index
            const uint16_t IDX0 = 0;
            // Request max 255 bytes for string (strings are short)
            auto str_cb = [this, client_ptr, idx](const esphome::usb_host::TransferStatus &st4) {
              if (st4.success && st4.data_len > 0) {
                auto &set3 = this->desc_cache_[client_ptr];
                set3.strings[idx] = std::vector<uint8_t>(st4.data, st4.data + st4.data_len);
                ESP_LOGI(USB_HOST_TAG, "Cached string descriptor idx=%u len=%u", (unsigned)idx, (unsigned)st4.data_len);
              } else {
                ESP_LOGW(USB_HOST_TAG, "String descriptor idx=%u fetch failed", (unsigned)idx);
              }
            };
            std::vector<uint8_t> dummy_str(255);
            auto client3 = static_cast<esphome::usb_host::USBClient *>(client_ptr);
            client3->control_transfer(bmReqStr, REQ_GET_DESCRIPTOR, VALUE_STR, IDX0, str_cb, dummy_str);
          };

          request_string(iManufacturer);
          request_string(iProduct);
          request_string(iSerial);
        }
      } else {
        ESP_LOGW(USB_HOST_TAG, "GET_DESCRIPTOR failed or empty for client");
      }
    };

    // Attempt the control transfer. We pass a dummy data vector sized to LENGTH
    // to indicate the expected transfer length for IN transfers (implementation dependent).
    std::vector<uint8_t> dummy(LENGTH);
    bool ok = client->control_transfer(bmRequestType, REQUEST_GET_DESCRIPTOR, VALUE_DEVICE_DESCRIPTOR, INDEX, cb, dummy);
    if (!ok) {
      ESP_LOGW(USB_HOST_TAG, "control_transfer call to request descriptor returned false (client may not be ready)");
    }
  }

  bool get_device_descriptor(void *client_ptr, std::vector<uint8_t> &out) override {
    auto it = this->desc_cache_.find(client_ptr);
    if (it == this->desc_cache_.end()) return false;
    out = it->second.device;
    return true;
  }

 protected:
  struct DescriptorSet {
    std::vector<uint8_t> device;
    std::vector<uint8_t> config;
    std::unordered_map<int, std::vector<uint8_t>> strings;
  };

  std::unordered_map<void *, DescriptorSet> desc_cache_{};
 protected:
  esphome::usb_host::USBHost *host_{nullptr};
};

std::unique_ptr<USBHostAdapter> make_esphome_usb_host_adapter(esphome::usb_host::USBHost *host) {
  return std::unique_ptr<USBHostAdapter>(new EsphomeUsbHostAdapter(host));
}

}  // namespace usbip
}  // namespace esphome
#endif
