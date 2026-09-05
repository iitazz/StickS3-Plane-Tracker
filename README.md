# M5StickS3 Plane Tracker

Native Arduino/PlatformIO firmware for an M5StickS3. It polls OpenSky for aircraft within 65 km of FCO/Fiumicino, sorts them by distance, and shows them on a compact radar display.

## Build

1. Open this folder in VS Code with PlatformIO installed.
2. Run `pio run -t upload`.
3. Connect to the `PlaneTracker-Setup` network if Wi-Fi has not been configured yet. Open `http://192.168.4.1` and save the Wi-Fi credentials.

Build artifacts are written to `builds/m5sticks3/`. The main firmware image is `builds/m5sticks3/plane-tracking.bin`.

## Live debugging

After uploading, open the serial monitor at `115200`. The firmware prints a URL such as `http://192.168.1.42/`. Open that URL from a device on the same Wi-Fi network to view live status, HTTP response code, content type, JSON parser errors, response preview, uptime, and aircraft counts. The page refreshes every five seconds.

Briefly press Button B to refresh aircraft data. Hold Button B for about half a second to show the device IP and web UI address on the display for five seconds.

The device starts at the main app menu by default, while the Web UI server is already active in the background. Open the device IP from any browser on the same Wi-Fi network. If no Wi-Fi credentials are configured, the device creates a setup network named `PlaneTracker-Setup` with password `planeconfig`; connect to it and open `http://192.168.4.1` to configure Wi-Fi. Saving Wi-Fi credentials reboots the device and attempts the new connection.

Button guide:

- Top button: next item or next plane; double-press for the previous option/plane; hold to exit the current app.
- Blue button beside the display: select/confirm in the menu; refresh aircraft data with a short press; hold to show the device IP.

The device menu contains two rows: `PLANE RADAR` and `WEB UI`. Use the top button to move through both rows and the blue button to select the highlighted row. To disable Web UI from the device, open the `WEB UI` app and press the blue button; the Web UI screen also provides the disable action in the browser.

The top button uses a short single click for `Next`, a double-click for `Previous`, and a hold to `Exit` the current app. In Plane Radar, these select the next or previous plane. The blue button uses a short single click for `Select` or `Refresh`, and a hold for the IP overlay. These gestures are consistent across the menu, Plane Radar, and Web UI.

## Web UI firmware updates

The first installation must be uploaded over USB. After that, build a new firmware image with `pio run`, open the device Web UI, choose the `builds/m5sticks3/plane-tracking.bin` file under `Firmware update`, and upload it. The device reboots automatically after a successful update.

The Web UI has no authentication, so firmware updates should only be performed from a trusted network. Anyone who can access the page can upload firmware and change device settings.

The aircraft information panel is on the right side of the radar. The Web UI includes an `Auto rotate` option. When enabled, gyro movement triggers an orientation check and the accelerometer determines the stable landscape rotation. Auto-rotation only uses the two landscape modes and never switches to portrait. The blue button disables Web UI when pressed inside the Web UI app; the top button exits back to the menu.

The web page also lets you change the airport label, scan-center latitude and longitude, scan radius in kilometers, and data refresh interval. Settings are stored in flash and survive reboot. The display redraws four times per second and the default data refresh is 30 seconds; both the display timing and initial defaults are in [include/config.h](include/config.h).

