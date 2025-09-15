#include "usbip.h"
#include "esphome/core/log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdio>
#ifdef ESP_PLATFORM
#include "esphome/components/usb_host/usb_host.h"
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
  if (!this->server_started_) {
    this->start_server();
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
    } else if (r == 0) {
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

  // Try to update cached descriptors
  this->update_client_descriptors();
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
  if (client_ptr) this->exported_clients_.push_back(client_ptr);
}

}  // namespace usbip
}  // namespace esphome
