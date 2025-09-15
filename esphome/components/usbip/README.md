# USBIP Custom Component for ESPHome

This is a minimal scaffold for a USB/IP server component for ESPHome — intended for use on ESP32-S3 devices that provide USB device functionality.

Features
- Component skeleton (setup, loop, dump_config)
- Basic configuration option: port (TCP port to listen on)

Usage

In your ESPHome YAML, add:

external_components:
  - source: local
    components: [usbip]

Example:

external_components:
  - source: local
    components: [usbip]

usbip:
  port: 3240

Notes
- This is only a scaffold. You'll need to implement the USB/IP server protocol handling and expose the ESP32-S3 USB device descriptors appropriately.
- Make sure the target chip supports USB device mode (ESP32-S3) and that USB drivers are enabled in your build.
