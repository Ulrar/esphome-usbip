#include "usbip.h"
#include "esphome/core/log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#ifdef ESP_PLATFORM
#include "esphome/components/usb_host/usb_host.h"
#include "esp_timer.h"
#endif

namespace esphome {
namespace usbip {

static const char *TAG = "usbip";

void USBIPComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up USB/IP server (port=%u)", this->port_);
  ESP_LOGI(TAG, "USBIPComponent setup() entering");
  // Ensure we have a host adapter. Prefer an ESP-IDF-backed adapter when
  // compiling for ESP; otherwise fall back to the dummy adapter for testing.
#ifdef ESP_PLATFORM
  if (!this->host_) {
    this->host_ = make_esp_idf_usb_host();
  }
#else
  if (!this->host_) {
    this->host_ = make_dummy_usb_host();
  }
#endif

  if (this->host_) {
    ESP_LOGI(TAG, "Starting host adapter...");
    if (!this->host_->begin()) {
      ESP_LOGE(TAG, "USB host adapter failed to start");
      // Continue; USB functionality will be disabled but TCP server may still be useful
    } else {
      ESP_LOGI(TAG, "Host adapter started successfully");
    }
  }

  // Request descriptors for any registered clients
  this->client_descriptors_.resize(this->exported_clients_.size());
  this->last_string_request_ms_.resize(this->exported_clients_.size());
  this->request_client_descriptors();
}

void USBIPComponent::start_server() {
  if (this->server_started_) return;
  ESP_LOGI(TAG, "Starting TCP server on port %u", this->port_);
  // Create a non-blocking TCP listening socket
  this->server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (this->server_fd_ < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    return;
  }

  int flags = fcntl(this->server_fd_, F_GETFL, 0);
  if (flags >= 0) fcntl(this->server_fd_, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(this->port_);

  int opt = 1;
  setsockopt(this->server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  if (bind(this->server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed: %d", errno);
    close(this->server_fd_);
    this->server_fd_ = -1;
    return;
  }

  if (listen(this->server_fd_, 1) < 0) {
    ESP_LOGE(TAG, "listen() failed: %d", errno);
    close(this->server_fd_);
    this->server_fd_ = -1;
    return;
  }

  ESP_LOGI(TAG, "Listening for USB/IP clients on port %u", this->port_);
  this->server_started_ = true;
}

void USBIPComponent::set_esphome_host(void *host_ptr) {
#ifdef ESP_PLATFORM
  auto host = static_cast<esphome::usb_host::USBHost *>(host_ptr);
  this->host_ = make_esphome_usb_host_adapter(host);
  ESP_LOGI(TAG, "Bound esphome usb_host instance to USB/IP component");
#else
  (void)host_ptr;
  ESP_LOGW(TAG, "set_esphome_host called but not compiled for ESP_PLATFORM");
#endif
}

void USBIPComponent::loop() {
  // Ensure TCP server is started from the first loop iterations
  // Helper to get current time in milliseconds (portable)
  auto now_ms = []() -> uint32_t {
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
#else
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
  };

  if (!this->server_started_) {
    this->start_server();
  }

  // Flush any pending send buffer in a non-blocking way (do this early in loop)
  if (this->sending_devlist_ && !this->send_buf_.empty() && this->client_fd_ >= 0) {
    const size_t CHUNK = 512; // smaller chunk to yield frequently
    size_t remaining = this->send_buf_.size() - this->send_offset_;
    size_t to_send = std::min(CHUNK, remaining);
    ssize_t s = send(this->client_fd_, this->send_buf_.data() + this->send_offset_, to_send, 0);
    if (s > 0) {
      this->send_offset_ += (size_t)s;
      if (this->send_offset_ >= this->send_buf_.size()) {
        ESP_LOGI(TAG, "Finished non-blocking send of devlist (total=%u)", (unsigned)this->send_buf_.size());
        this->send_buf_.clear();
        this->send_offset_ = 0;
        this->sending_devlist_ = false;
        this->pending_devlist_ = false;
      }
    } else if (s < 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        ESP_LOGW(TAG, "send() failed while flushing devlist: %d", errno);
        close(this->client_fd_);
        this->client_fd_ = -1;
        this->send_buf_.clear();
        this->send_offset_ = 0;
        this->sending_devlist_ = false;
        this->pending_devlist_ = false;
      }
    }
  }

  if (this->server_fd_ < 0) {
    // Server not available yet; still poll host and update descriptors
    if (this->host_) this->host_->poll();
    this->update_client_descriptors();
    return;
  }

  // Accept a single client (non-blocking)
  if (this->client_fd_ < 0) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int fd = accept(this->server_fd_, (struct sockaddr *)&client_addr, &client_len);
    if (fd >= 0) {
      // Set non-blocking on client
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      this->client_fd_ = fd;
      ESP_LOGI(TAG, "Accepted client %s:%u", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    } else {
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        // Unexpected error
        // Log at debug level to avoid flooding
        ESP_LOGD(TAG, "accept() returned %d (errno=%d)", fd, errno);
      }
    }
  }

  // Read from client if present
  if (this->client_fd_ >= 0) {
    uint8_t buf[512];
    ssize_t r = recv(this->client_fd_, buf, sizeof(buf), 0);
    if (r > 0) {
      ESP_LOGD(TAG, "Received %d bytes from client", (int)r);
      // Check for OP_REQ_DEVLIST (USB/IP) and reply with a minimal OP_REP_DEVLIST
      // If the client sent at least 4 bytes, interpret the first 4 as the command (network byte order)
      bool handled = false;
  if (r >= 4) {
        // USB/IP request header starts with two 16-bit fields: version and command
        uint16_t ver_net;
        uint16_t cmd_net16;
        memcpy(&ver_net, buf + 0, 2);
        memcpy(&cmd_net16, buf + 2, 2);
        uint16_t ver = ntohs(ver_net);
        uint16_t cmd16 = ntohs(cmd_net16);
        // OP_REQ_DEVLIST command code is 0x8005
        if (cmd16 == 0x8005) {
          ESP_LOGI(TAG, "Received OP_REQ_DEVLIST (ver=0x%04X) from usbip client", ver);
          // Initiate asynchronous descriptor requests; complete the reply in
          // later loop() iterations when descriptors are ready or timeout
          // expires to avoid long blocking here.
          for (auto cptr : this->exported_clients_) {
            std::vector<uint8_t> tmp;
            if (!this->host_->get_device_descriptor(cptr, tmp)) {
              this->host_->request_device_descriptor(cptr);
            }
          }
            // Mark pending and set a deadline (ms since boot). The wait time
            // is configurable via set_string_wait_ms(); keep a short default
            // to avoid blocking the client too long.
            this->pending_devlist_ = true;
            this->pending_devlist_deadline_ = now_ms() + this->string_wait_ms_;
          handled = true;
        }
      }
      if (!handled) {
      // For now, just log hex of first bytes
      std::string s;
      s.reserve(r * 3);
      for (ssize_t i = 0; i < r; ++i) {
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "%02X ", buf[i]);
        s += tmp;
      }
      ESP_LOGI(TAG, "Client data (hex): %s", s.c_str());

      // Echo back to client (simple behavior to test round-trip)
      ssize_t w = send(this->client_fd_, buf, r, 0);
      if (w < 0) {
        ESP_LOGW(TAG, "send() failed: %d", errno);
      }
      }
    }
    else if (r == 0) {
      ESP_LOGI(TAG, "Client disconnected");
      close(this->client_fd_);
      this->client_fd_ = -1;
    } else {
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        ESP_LOGW(TAG, "recv() error: %d", errno);
        close(this->client_fd_);
        this->client_fd_ = -1;
      }
    }
  }
  
  // Poll USB host if available
  if (this->host_) {
    this->host_->poll();
  }

  // Flush any pending send buffer in a non-blocking way
  if (this->sending_devlist_ && !this->send_buf_.empty() && this->client_fd_ >= 0) {
    const size_t CHUNK = 1024;
    size_t remaining = this->send_buf_.size() - this->send_offset_;
    size_t to_send = std::min(CHUNK, remaining);
    ssize_t s = send(this->client_fd_, this->send_buf_.data() + this->send_offset_, to_send, 0);
    if (s > 0) {
      this->send_offset_ += (size_t)s;
      if (this->send_offset_ >= this->send_buf_.size()) {
        ESP_LOGI(TAG, "Finished non-blocking send of devlist (total=%u)", (unsigned)this->send_buf_.size());
        this->send_buf_.clear();
        this->send_offset_ = 0;
        this->sending_devlist_ = false;
        this->pending_devlist_ = false;
      }
    } else if (s < 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        ESP_LOGW(TAG, "send() failed while flushing devlist: %d", errno);
        close(this->client_fd_);
        this->client_fd_ = -1;
        this->send_buf_.clear();
        this->send_offset_ = 0;
        this->sending_devlist_ = false;
        this->pending_devlist_ = false;
      }
    }
  }

  // Try to update cached descriptors
  this->update_client_descriptors();

  if (this->pending_devlist_) {
    bool all_device_ready = true;
    for (auto cptr : this->exported_clients_) {
      std::vector<uint8_t> tmp;
      if (!this->host_->get_device_descriptor(cptr, tmp)) { all_device_ready = false; break; }
    }

    // Check whether required strings (iManufacturer/iProduct) are cached.
    bool all_strings_ready = true;
    for (auto cptr : this->exported_clients_) {
      std::vector<uint8_t> devd;
      if (!this->host_->get_device_descriptor(cptr, devd) || devd.size() < 16) { all_strings_ready = false; break; }
      int iManufacturer = devd[14];
      int iProduct = devd[15];
      std::vector<uint8_t> tmp;
      if (iManufacturer > 0 && !this->host_->get_string_descriptor(cptr, iManufacturer, tmp)) { all_strings_ready = false; break; }
      if (iProduct > 0 && !this->host_->get_string_descriptor(cptr, iProduct, tmp)) { all_strings_ready = false; break; }
    }

    // While waiting for the devlist deadline, issue conservative, rate-limited
    // retries for missing string descriptors so they may be available when we
    // compose the reply. We avoid busy-waiting by checking last attempt times.
    if ((!all_device_ready || !all_strings_ready) && now_ms() < this->pending_devlist_deadline_) {
      // For each client, find string indices and request them if sufficient
      for (size_t ci = 0; ci < this->exported_clients_.size(); ++ci) {
        void *cptr = this->exported_clients_[ci];
        std::vector<uint8_t> devd;
        if (!this->host_->get_device_descriptor(cptr, devd) || devd.size() < 16) continue;
        int iManufacturer = devd[14];
        int iProduct = devd[15];
        auto try_request = [&](int idx) {
          if (idx <= 0) return;
          uint32_t now = now_ms();
          auto &map = this->last_string_request_ms_[ci];
          auto it = map.find(idx);
          if (it == map.end() || now - it->second >= this->string_request_interval_ms_) {
            // issue a non-blocking request (adapter will handle retries/fallback)
            this->host_->request_string_descriptor(cptr, idx);
            map[idx] = now;
          }
        };
        try_request(iManufacturer);
        try_request(iProduct);
      }
      // Not yet time to finish; return and let retries progress
      // Poll host and come back
      if (this->host_) this->host_->poll();
      this->update_client_descriptors();
      return;
    }

  if ((all_device_ready && all_strings_ready) || now_ms() >= this->pending_devlist_deadline_) {
  // Send the OP_REP_DEVLIST header and device records (same logic as before)
      uint16_t rep_ver_net = htons(0x0111);
      uint16_t rep_cmd_net = htons(0x0005);
      uint32_t status_net = htonl(0);

      uint32_t ndev = (uint32_t)this->exported_clients_.size();
      uint32_t ndev_net = htonl(ndev);

  // Build reply into send buffer for non-blocking send
  this->send_buf_.clear();
  this->send_offset_ = 0;
  this->sending_devlist_ = true;
  // Header (network-order copies)
  this->send_buf_.insert(this->send_buf_.end(), (uint8_t *)&rep_ver_net, (uint8_t *)&rep_ver_net + 2);
  this->send_buf_.insert(this->send_buf_.end(), (uint8_t *)&rep_cmd_net, (uint8_t *)&rep_cmd_net + 2);
  this->send_buf_.insert(this->send_buf_.end(), (uint8_t *)&status_net, (uint8_t *)&status_net + 4);
  this->send_buf_.insert(this->send_buf_.end(), (uint8_t *)&ndev_net, (uint8_t *)&ndev_net + 4);
  ESP_LOGI(TAG, "Queued OP_REP_DEVLIST header (n=%u)", ndev);
      for (size_t i = 0; i < this->exported_clients_.size(); ++i) {
        void *c = this->exported_clients_[i];
          std::vector<uint8_t> dev_desc;
          uint16_t idVendor = 0, idProduct = 0, bcdDevice = 0;
          uint8_t devClass = 0, devSub = 0, devProto = 0, bNumConfigurations = 0;
          if (this->host_->get_device_descriptor(c, dev_desc) && dev_desc.size() >= 1) {
            // Log raw device descriptor bytes for debugging
            std::string hex;
            size_t show = std::min((size_t)18, dev_desc.size());
            hex.reserve(show * 3);
            for (size_t bi = 0; bi < show; ++bi) {
              char tmp[4];
              snprintf(tmp, sizeof(tmp), "%02X ", dev_desc[bi]);
              hex += tmp;
            }
            ESP_LOGD(TAG, "Device descriptor bytes (first %u): %s", (unsigned)show, hex.c_str());
            if (dev_desc.size() >= 18) {
              // Must have full 18-byte device descriptor to parse ids
              int iManufacturer = dev_desc[14];
              int iProduct = dev_desc[15];
              idVendor = (uint16_t)dev_desc[8] | ((uint16_t)dev_desc[9] << 8);
              idProduct = (uint16_t)dev_desc[10] | ((uint16_t)dev_desc[11] << 8);
              // Log whether these strings are already cached to help tuning
              std::vector<int> missing_indices;
              std::vector<uint8_t> tmp;
              if (iManufacturer > 0 && !this->host_->get_string_descriptor(c, iManufacturer, tmp)) missing_indices.push_back(iManufacturer);
              if (iProduct > 0 && !this->host_->get_string_descriptor(c, iProduct, tmp)) missing_indices.push_back(iProduct);
              if (!missing_indices.empty()) {
                std::string ms;
                for (auto idx : missing_indices) {
                  char t[8]; snprintf(t, sizeof(t), "%d ", idx); ms += t;
                }
                ESP_LOGD(TAG, "Device %u missing string indices: %s", (unsigned)i, ms.c_str());
              }
              bcdDevice = (uint16_t)dev_desc[12] | ((uint16_t)dev_desc[13] << 8);
              devClass = dev_desc[4];
              devSub = dev_desc[5];
              devProto = dev_desc[6];
              bNumConfigurations = dev_desc[17];
            } else {
              ESP_LOGW(TAG, "Device descriptor too short (%u bytes)", (unsigned)dev_desc.size());
            }
            ESP_LOGI(TAG, "Parsed idVendor=0x%04X idProduct=0x%04X", (unsigned)idVendor, (unsigned)idProduct);
          }

          std::vector<uint8_t> rec;
          rec.resize(256 + 32 + (16 * 4));
          const char *path = "/";
          strncpy((char *)rec.data(), path, 255);
          char busid[32];
          snprintf(busid, sizeof(busid), "1-%u", (unsigned)(i + 1));
          strncpy((char *)(rec.data() + 256), busid, 31);
          size_t num_base = 256 + 32;
          auto put_u32 = [&](size_t idx, uint32_t v) {
            uint32_t tmp = htonl(v);
            memcpy(rec.data() + num_base + idx * 4, &tmp, 4);
          };
          put_u32(0, 0);
          put_u32(1, (uint32_t)(i + 1));
          put_u32(2, 3);
          // Place vendor/product in the canonical order expected by USB/IP
          // clients: idVendor then idProduct.
          put_u32(3, (uint32_t)idVendor);
          put_u32(4, (uint32_t)idProduct);
          put_u32(5, (uint32_t)bcdDevice);
          put_u32(6, (uint32_t)devClass);
          put_u32(7, (uint32_t)devSub);
          put_u32(8, (uint32_t)devProto);
          put_u32(9, 1);
          put_u32(10, 0);
          put_u32(11, 0);
          put_u32(12, 0);
          put_u32(13, 0);
          put_u32(14, 0);
          put_u32(15, (uint32_t)(bNumConfigurations ? bNumConfigurations : 1));

          std::vector<uint8_t> extra;
          if (!dev_desc.empty()) {
            uint32_t dlen = (uint32_t)dev_desc.size();
            uint32_t dlen_net = htonl(dlen);
            extra.insert(extra.end(), (uint8_t *)&dlen_net, (uint8_t *)&dlen_net + 4);
            extra.insert(extra.end(), dev_desc.begin(), dev_desc.end());
          } else {
            uint32_t dlen_net = htonl(0);
            extra.insert(extra.end(), (uint8_t *)&dlen_net, (uint8_t *)&dlen_net + 4);
          }
          std::vector<uint8_t> cfg;
          if (this->host_->get_config_descriptor(c, cfg) && !cfg.empty()) {
            uint32_t clen = (uint32_t)cfg.size();
            uint32_t clen_net = htonl(clen);
            extra.insert(extra.end(), (uint8_t *)&clen_net, (uint8_t *)&clen_net + 4);
            extra.insert(extra.end(), cfg.begin(), cfg.end());
          } else {
            uint32_t clen_net = htonl(0);
            extra.insert(extra.end(), (uint8_t *)&clen_net, (uint8_t *)&clen_net + 4);
          }

          // Append iManufacturer and iProduct string descriptors (if available)
          if (dev_desc.size() >= 16) {
            int iManufacturer = dev_desc[14];
            int iProduct = dev_desc[15];
            auto append_string_index = [&](int idx) {
              if (idx <= 0) {
                uint32_t slen_net = htonl(0);
                extra.insert(extra.end(), (uint8_t *)&slen_net, (uint8_t *)&slen_net + 4);
                return;
              }
              std::vector<uint8_t> sraw;
              if (!this->host_->get_string_descriptor(c, idx, sraw)) {
                // Request asynchronously for future calls
                this->host_->request_string_descriptor(c, idx);
                uint32_t slen_net = htonl(0);
                extra.insert(extra.end(), (uint8_t *)&slen_net, (uint8_t *)&slen_net + 4);
                return;
              }
              // sraw is a USB string descriptor (bLength, bDescriptorType, UTF-16LE chars)
              if (sraw.size() < 2) {
                uint32_t slen_net = htonl(0);
                extra.insert(extra.end(), (uint8_t *)&slen_net, (uint8_t *)&slen_net + 4);
                return;
              }
              // Convert UTF-16LE to UTF-8 (simple implementation for BMP/basic ascii)
              std::string utf8;
              for (size_t si = 2; si + 1 < sraw.size(); si += 2) {
                uint16_t ch = sraw[si] | (sraw[si + 1] << 8);
                if (ch < 0x80) {
                  utf8.push_back((char)ch);
                } else if (ch < 0x800) {
                  utf8.push_back((char)(0xC0 | ((ch >> 6) & 0x1F)));
                  utf8.push_back((char)(0x80 | (ch & 0x3F)));
                } else {
                  utf8.push_back((char)(0xE0 | ((ch >> 12) & 0x0F)));
                  utf8.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
                  utf8.push_back((char)(0x80 | (ch & 0x3F)));
                }
              }
              uint32_t slen = (uint32_t)utf8.size();
              uint32_t slen_net = htonl(slen);
              extra.insert(extra.end(), (uint8_t *)&slen_net, (uint8_t *)&slen_net + 4);
              extra.insert(extra.end(), utf8.begin(), utf8.end());
            };

            append_string_index(iManufacturer);
            append_string_index(iProduct);
          } else {
            // two zero-length string entries
            uint32_t slen_net = htonl(0);
            extra.insert(extra.end(), (uint8_t *)&slen_net, (uint8_t *)&slen_net + 4);
            extra.insert(extra.end(), (uint8_t *)&slen_net, (uint8_t *)&slen_net + 4);
          }

          // Debug: dump numeric fields (16 u32) to help diagnose endianness/offsets
          {
            std::string numhex;
            numhex.reserve(16 * 11);
            for (size_t bi = 0; bi < 16 * 4; ++bi) {
              char tmp[4];
              snprintf(tmp, sizeof(tmp), "%02X ", rec[num_base + bi]);
              numhex += tmp;
            }
            ESP_LOGD(TAG, "Device record numeric fields (hex): %s", numhex.c_str());

            // Also decode each u32 (network order -> host order) to show values
            std::string vals;
            vals.reserve(16 * 12);
            for (size_t idx = 0; idx < 16; ++idx) {
              uint32_t netv = 0;
              memcpy(&netv, rec.data() + num_base + idx * 4, 4);
              uint32_t hostv = ntohl(netv);
              char tmp[32];
              snprintf(tmp, sizeof(tmp), "%02zu:%08X ", idx, hostv);
              vals += tmp;
            }
            ESP_LOGD(TAG, "Device record numeric fields (decoded): %s", vals.c_str());
          }

          // Append record and extra blobs to send buffer (non-blocking send)
          this->send_buf_.insert(this->send_buf_.end(), rec.begin(), rec.end());
          if (!extra.empty()) this->send_buf_.insert(this->send_buf_.end(), extra.begin(), extra.end());
          ESP_LOGI(TAG, "Queued device record %u (len=%u + extra=%u)", (unsigned)i, (unsigned)rec.size(), (unsigned)extra.size());
        }
      }
      // pending_devlist_ remains true until send buffer completely flushed
    }
  }
}

void USBIPComponent::request_client_descriptors() {
  if (!this->host_) return;
  for (auto c : this->exported_clients_) {
    this->host_->request_device_descriptor(c);
  }
}

void USBIPComponent::update_client_descriptors() {
  if (!this->host_) return;
  for (size_t i = 0; i < this->exported_clients_.size(); ++i) {
    auto c = this->exported_clients_[i];
    std::vector<uint8_t> desc;
    if (this->host_->get_device_descriptor(c, desc)) {
      if (desc != this->client_descriptors_[i]) {
        this->client_descriptors_[i] = std::move(desc);
        ESP_LOGI(TAG, "Cached device descriptor for client %u (len=%u)", (unsigned)i,
                 (unsigned)this->client_descriptors_[i].size());
        // Proactively request iManufacturer/iProduct strings (non-blocking).
        if (this->client_descriptors_[i].size() >= 16) {
          int iManufacturer = this->client_descriptors_[i][14];
          int iProduct = this->client_descriptors_[i][15];
          if (iManufacturer > 0) this->host_->request_string_descriptor(c, iManufacturer);
          if (iProduct > 0) this->host_->request_string_descriptor(c, iProduct);
        }
      }
    }
  }
}

void USBIPComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "USB/IP server:");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);

  if (!this->exported_clients_.empty()) {
    ESP_LOGCONFIG(TAG, "  Exported USB clients: %u", (unsigned)this->exported_clients_.size());
#ifdef ESP_PLATFORM
    for (auto c : this->exported_clients_) {
      auto client = static_cast<esphome::usb_host::USBClient *>(c);
      if (client) {
        client->dump_config();
      }
    }
#else
    ESP_LOGCONFIG(TAG, "    (client info only available on ESP platform)");
#endif
  }
}

void USBIPComponent::add_exported_client(void *client_ptr) {
  if (client_ptr) {
    this->exported_clients_.push_back(client_ptr);
    // Keep the last-request map in sync with clients
    this->last_string_request_ms_.emplace_back();
    this->client_descriptors_.resize(this->exported_clients_.size());
  }
}

}  // namespace usbip
}  // namespace esphome
