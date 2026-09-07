# Universal Plane Tracker (M5StickS3 & Freenove ESP32-S3 Display)

> **AI disclosure:** AI tools were used to assist with parts of this project. Anyone installing or running this firmware is responsible for understanding and accepting what it does on their device.

Universal Arduino/PlatformIO firmware that runs on both **M5StickS3** and **Freenove ESP32-S3 Display**. The firmware automatically detects the hardware at boot and configures the display drivers, buttons, touchscreen, and power management dynamically!

## Build and flash

Open the project in VS Code with PlatformIO installed, then run:

```bash
pio run -t upload
```

The universal firmware image is generated at [builds/universal/plane-tracking.bin](builds/universal/plane-tracking.bin). You can flash this exact same `.bin` file to either an M5StickS3 or a Freenove ESP32-S3 Display.

## Controls & Board Features

The UI and control prompts automatically adapt based on the detected hardware:

### Physical Buttons (when running on M5StickS3)
* **Top Button (BtnB)**: Single press for next plane / menu item; double press for previous plane; hold to pause/resume radar.
* **Blue Button (BtnA)**: Select menu item / refresh planes; hold to toggle IP overlay.
* **PMIC & IMU**: Reads battery percentage and auto-rotates screen on accelerometer motion.

### Touch Gestures (when running on Freenove ESP32-S3 Display)
* **Swipe Left / Swipe Right**: Cycle to next / previous plane on radar.
* **Single Tap Center / Soft Buttons**: Refresh planes / select menu options.
* **Tap Top Header Bar**: Toggle IP overlay.
* **Long Press Screen (>600ms)**: Pause / resume radar.

## Web UI & Configuration

If Wi-Fi is not configured, connect to `PlaneTracker-Setup` using password `planeconfig`, then open `http://192.168.4.1`. The Web UI allows you to:
* Scan for nearby Wi-Fi networks and save credentials.
* Configure airport presets or manual latitude/longitude coordinates.
* Adjust radar range (km) and refresh intervals.
* Toggle display auto-rotation.
* **OpenSky API Credentials (Optional)**: By default, the device uses the public OpenSky API in anonymous mode with standard rate limits (400 requests/day). Providing OpenSky OAuth2 Client Credentials (`Client ID` and `Client Secret`) is **completely optional** and only needed if you want higher daily API request limits (4,000 requests/day) for more frequent updates. Client Secrets are encrypted on device NVS using unique ESP32 silicon eFuse keys and are never exposed in Web UI API responses.

## OTA updates

Flash the OTA-enabled firmware over USB once. For later updates, run `pio run`, open the device Web UI, select `builds/universal/plane-tracking.bin`, and upload it. The device shows upload progress and waits for a button/touch confirmation before rebooting into the new firmware.

The Web UI also provides an `Install latest GitHub release` option. It downloads the latest attached `plane-tracking.bin` asset from the repository's GitHub release, installs it into the inactive OTA partition, and reboots.

The Web UI has no authentication. Use it only on a trusted network because anyone with access can change settings or upload firmware.

Default settings and display timing are defined in [include/config.h](include/config.h).

