# M5StickS3 Plane Tracker

> **AI disclosure:** AI tools were used to assist with parts of this project. Anyone installing or running this firmware is responsible for understanding and accepting what it does on their device.

Arduino/PlatformIO firmware for the M5StickS3. It displays nearby aircraft from OpenSky on a radar centered on a configurable airport or latitude/longitude.

## Build and flash

Open the project in VS Code with PlatformIO installed, then run:

```bash
pio run -t upload
```

The firmware image is generated at `builds/m5sticks3/plane-tracking.bin`.

## Use

The device starts in the launcher. The top button moves between `PLANE RADAR` and `WEB UI`; the blue button selects. In Plane Radar, the top button selects the next plane, double-press selects the previous plane, the blue button refreshes data, and holding it shows the device address. Hold the top button to return to the launcher.

If Wi-Fi is not configured, connect to `PlaneTracker-Setup` using password `planeconfig`, then open `http://192.168.4.1`. The Web UI can scan for nearby networks, select an SSID, configure an airport or manual coordinates, change the radar range shown on the device, and enable auto-rotation.

## OTA updates

Flash the OTA-enabled firmware over USB once. For later updates, run `pio run`, open the device Web UI, select `builds/m5sticks3/plane-tracking.bin`, and upload it. The device shows upload progress and waits for a blue-button press before rebooting into the new firmware.

The Web UI has no authentication. Use it only on a trusted network because anyone with access can change settings or upload firmware.

Default settings and display timing are defined in [include/config.h](include/config.h).

