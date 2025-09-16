#include "usb_host.h"

#ifdef ESP_PLATFORM
#include "esphome/components/usb_host/usb_host.h"
#include <cstring>
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
    auto extract_descriptor = [](const uint8_t *data, size_t len, uint8_t dtype, size_t minlen) -> std::vector<uint8_t> {
      // Look for a descriptor where bDescriptorType == dtype and bLength >= minlen
      for (size_t off = 0; off + 2 <= len; ++off) {
        uint8_t bl = data[off];
        uint8_t bt = data[off + 1];
        if (bt == dtype && bl >= minlen && off + bl <= len) {
          return std::vector<uint8_t>(data + off, data + off + bl);
        }
      }
      return {};
    };

    auto cb = [this, client_ptr, extract_descriptor](const esphome::usb_host::TransferStatus &st) {
      if (st.success && st.data && st.data_len > 0) {
        // Try to extract a proper device descriptor (type 1, length >= 18)
        std::vector<uint8_t> v = extract_descriptor(st.data, st.data_len, 1, 18);
        if (v.empty()) {
          // Fallback: if the buffer length equals 18 or more, assume the start is descriptor
          if (st.data_len >= 18) v = std::vector<uint8_t>(st.data, st.data + std::min((size_t)st.data_len, (size_t)18));
        }
        if (!v.empty()) {
          auto &set = this->desc_cache_[client_ptr];
          set.device = v;
          ESP_LOGI(USB_HOST_TAG, "Received %u bytes device descriptor for client (cached %u)", (unsigned)st.data_len, (unsigned)v.size());
          if (v.size() >= 18) {
            uint8_t iManufacturer = v[14];
            uint8_t iProduct = v[15];
            uint8_t iSerial = v[16];
            uint8_t bNumConfigurations = v[17];
            (void)iManufacturer; (void)iProduct; (void)iSerial; (void)bNumConfigurations;
          }
        } else {
          ESP_LOGW(USB_HOST_TAG, "GET_DESCRIPTOR returned data but no device descriptor found (len=%u)", (unsigned)st.data_len);
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

  void request_config_descriptor(void *client_ptr) override {
    if (!client_ptr) return;
    auto client = static_cast<esphome::usb_host::USBClient *>(client_ptr);
    if (!client) return;

    // First request: read 9 bytes to learn wTotalLength
    uint8_t bmReq = esphome::usb_host::USB_DIR_IN | esphome::usb_host::USB_TYPE_STANDARD |
                    esphome::usb_host::USB_RECIP_DEVICE;
    const uint8_t REQ_GET_DESCRIPTOR = 0x06;
    const uint16_t VALUE_CFG_DESC = (2 << 8);
    const uint16_t INDEX0 = 0;

    auto probe_cb = [this, client_ptr, bmReq](const esphome::usb_host::TransferStatus &st) {
      if (!st.success || st.data_len < 9) {
        ESP_LOGW(USB_HOST_TAG, "Config probe failed");
        return;
      }
      uint16_t total_len = st.data[2] | (st.data[3] << 8);
      ESP_LOGI(USB_HOST_TAG, "Config total length=%u, fetching in segments", (unsigned)total_len);

      // Fetch in segments up to 255 bytes
      std::vector<uint8_t> full; full.resize(total_len);
      size_t offset = 0;
      const size_t chunk = 255;
      while (offset < total_len) {
        size_t want = std::min(chunk, (size_t)total_len - offset);
        std::vector<uint8_t> buf(want);
        auto seg_cb = [this, client_ptr, &full, offset](const esphome::usb_host::TransferStatus &st2) mutable {
          if (st2.success && st2.data_len > 0) {
            size_t copy_len = std::min((size_t)st2.data_len, full.size() - offset);
            memcpy(full.data() + offset, st2.data, copy_len);
          } else {
            ESP_LOGW(USB_HOST_TAG, "Config segment fetch failed at offset %u", (unsigned)offset);
          }
        };
        auto client2 = static_cast<esphome::usb_host::USBClient *>(client_ptr);
        client2->control_transfer(bmReq, REQ_GET_DESCRIPTOR, VALUE_CFG_DESC, INDEX0, seg_cb, buf);
        offset += want;
      }
      // Cache the assembled descriptor
      auto &set = this->desc_cache_[client_ptr];
      set.config = std::move(full);
      ESP_LOGI(USB_HOST_TAG, "Cached full configuration descriptor (%u bytes)", (unsigned)set.config.size());
    };

    std::vector<uint8_t> probe(9);
    client->control_transfer(bmReq, REQ_GET_DESCRIPTOR, VALUE_CFG_DESC, INDEX0, probe_cb, probe);
  }

  void request_string_descriptor(void *client_ptr, int index) override {
    if (!client_ptr || index <= 0) return;
    auto client = static_cast<esphome::usb_host::USBClient *>(client_ptr);
    if (!client) return;

    uint8_t bmReq = esphome::usb_host::USB_DIR_IN | esphome::usb_host::USB_TYPE_STANDARD |
                    esphome::usb_host::USB_RECIP_DEVICE;
    const uint8_t REQ_GET_DESCRIPTOR = 0x06;
    const uint16_t VALUE_STR_DESC = (3 << 8) | (index & 0xFF);
    const uint16_t INDEX0 = 0;

    // First fetch 2 bytes to read bLength
    auto extract_descriptor = [](const uint8_t *data, size_t len, uint8_t dtype, size_t minlen) -> std::vector<uint8_t> {
      for (size_t off = 0; off + 2 <= len; ++off) {
        uint8_t bl = data[off];
        uint8_t bt = data[off + 1];
        if (bt == dtype && bl >= minlen && off + bl <= len) {
          return std::vector<uint8_t>(data + off, data + off + bl);
        }
      }
      return {};
    };

    auto probe_cb = [this, client_ptr, index, bmReq, extract_descriptor](const esphome::usb_host::TransferStatus &st) {
      if (!st.success || st.data_len < 2) {
        ESP_LOGW(USB_HOST_TAG, "String descriptor probe failed for index %d", index);
        return;
      }
      // Try to extract string descriptor (type 3)
      std::vector<uint8_t> found = extract_descriptor(st.data, st.data_len, 3, 2);
      if (found.empty()) {
        // No clean descriptor found; log first bytes for debugging
        std::string hex;
        size_t show = std::min((size_t)32, (size_t)st.data_len);
        hex.reserve(show * 3);
        for (size_t bi = 0; bi < show; ++bi) {
          char tmp[4];
          snprintf(tmp, sizeof(tmp), "%02X ", st.data[bi]);
          hex += tmp;
        }
        ESP_LOGW(USB_HOST_TAG, "String descriptor probe returned unexpected data (len=%u) for index %d: %s", (unsigned)st.data_len, index, hex.c_str());
        // Try an immediate retry. If the probe includes a partial descriptor
        // header (bLength, bDescriptorType) we can use the reported length to
        // request exactly that many bytes which is less likely to trigger host
        // stack errors than a large fixed buffer.
        size_t want = 0;
        for (size_t off = 0; off + 2 <= st.data_len; ++off) {
          uint8_t bl = st.data[off];
          uint8_t bt = st.data[off + 1];
          if (bt == 3 /* STRING */ && bl >= 2) {
            // If the descriptor would extend past the probe buffer, use it
            if (off + bl > st.data_len) {
              want = bl;
              break;
            }
          }
        }
  // Fallback conservative size when no hint found
  if (want == 0) want = 64;
  // Clamp to a safe maximum to avoid asking for absurdly-large transfers
  const size_t WANT_MAX = 512;
  if (want > WANT_MAX) want = WANT_MAX;
        auto retry_cb = [this, client_ptr, index, extract_descriptor](const esphome::usb_host::TransferStatus &st2) mutable {
          if (st2.success && st2.data_len > 0) {
            std::vector<uint8_t> v = extract_descriptor(st2.data, st2.data_len, 3, 2);
            if (v.empty()) v = std::vector<uint8_t>(st2.data, st2.data + st2.data_len);
            auto &set = this->desc_cache_[client_ptr];
            auto it = set.strings.find(index);
            if (it == set.strings.end() || it->second != v) {
              set.strings[index] = std::move(v);
              ESP_LOGI(USB_HOST_TAG, "Cached string descriptor index %d after retry (%u bytes)", index, (unsigned)set.strings[index].size());
            } else {
              // Already cached identical content; no new log
            }
          } else {
            // Immediate retry failed; schedule an async request instead and return
            ESP_LOGW(USB_HOST_TAG, "String descriptor retry failed for index %d; scheduling async fetch", index);
            auto client3 = static_cast<esphome::usb_host::USBClient *>(client_ptr);
            // Fire off an asynchronous request with a small probe; it will cache when ready
            std::vector<uint8_t> small(2);
            client3->control_transfer(esphome::usb_host::USB_DIR_IN | esphome::usb_host::USB_TYPE_STANDARD | esphome::usb_host::USB_RECIP_DEVICE,
                                      REQ_GET_DESCRIPTOR, (3 << 8) | (index & 0xFF), INDEX0,
                                      [this, client_ptr, index](const esphome::usb_host::TransferStatus &st3) {
                                        // The probe handler path will handle caching via another request
                                        (void)st3; (void)client_ptr; (void)index;
                                      }, small);
          }
        };
  std::vector<uint8_t> buf2(want);
        auto client2 = static_cast<esphome::usb_host::USBClient *>(client_ptr);
        bool ok = client2->control_transfer(bmReq, REQ_GET_DESCRIPTOR, (3 << 8) | (index & 0xFF), INDEX0, retry_cb, buf2);
        if (!ok) {
          ESP_LOGW(USB_HOST_TAG, "String descriptor retry control_transfer returned false for index %d; scheduling async fetch", index);
          // Schedule an async probe (small) to try later
          std::vector<uint8_t> small(2);
          client2->control_transfer(bmReq, REQ_GET_DESCRIPTOR, (3 << 8) | (index & 0xFF), INDEX0,
                                    [this, client_ptr, index](const esphome::usb_host::TransferStatus &st3) {
                                      (void)st3; (void)client_ptr; (void)index;
                                    }, small);
        }
        return;
      }
      // Request full descriptor using its reported length
      size_t want = found.size();
      auto seg_cb = [this, client_ptr, index, extract_descriptor](const esphome::usb_host::TransferStatus &st2) mutable {
          if (st2.success && st2.data_len > 0) {
          // Try to extract clean descriptor
          std::vector<uint8_t> v = extract_descriptor(st2.data, st2.data_len, 3, 2);
          if (v.empty()) v = std::vector<uint8_t>(st2.data, st2.data + st2.data_len);
          auto &set = this->desc_cache_[client_ptr];
          auto it = set.strings.find(index);
          if (it == set.strings.end() || it->second != v) {
            set.strings[index] = std::move(v);
            ESP_LOGI(USB_HOST_TAG, "Cached string descriptor index %d (%u bytes)", index, (unsigned)set.strings[index].size());
          } else {
            // Already cached identical content; suppress duplicate log
          }
        } else {
          ESP_LOGW(USB_HOST_TAG, "String descriptor fetch failed for index %d", index);
        }
      };
      std::vector<uint8_t> buf(want);
      auto client2 = static_cast<esphome::usb_host::USBClient *>(client_ptr);
      client2->control_transfer(bmReq, REQ_GET_DESCRIPTOR, (3 << 8) | (index & 0xFF), INDEX0, seg_cb, buf);
    };

    std::vector<uint8_t> probe(2);
    client->control_transfer(bmReq, REQ_GET_DESCRIPTOR, VALUE_STR_DESC, INDEX0, probe_cb, probe);
  }

  bool get_config_descriptor(void *client_ptr, std::vector<uint8_t> &out) override {
    auto it = this->desc_cache_.find(client_ptr);
    if (it == this->desc_cache_.end()) return false;
    out = it->second.config;
    return true;
  }

  bool get_string_descriptor(void *client_ptr, int index, std::vector<uint8_t> &out) override {
    auto it = this->desc_cache_.find(client_ptr);
    if (it == this->desc_cache_.end()) return false;
    auto sit = it->second.strings.find(index);
    if (sit == it->second.strings.end()) return false;
    out = sit->second;
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
