# T-Beam GPS Disciplined NTP Server

Firmware for a LilyGo/TTGO T-Beam v1.1 used as a GPS/PPS disciplined standalone IPv4 NTP server.

The primary audience is the ham radio community and anyone else who needs accurate local network time in remote, austere, or off-grid conditions. The device can run as its own WiFi access point, DHCP server, DNS helper, captive portal, and NTP server, or it can join an existing LAN as a normal WiFi client.

NTP itself is always UTC. Local time and daylight-saving settings only affect the captive portal and OLED display.

## What It Does

- Serves NTP on UDP/123 from GPS time, disciplined by the GPS PPS input on `GPIO37`.
- Fails closed: if recent GPS time is not available, NTP requests are counted but no time is handed out.
- Runs standalone AP mode by default at `192.168.4.1`.
- In AP mode, DHCP can advertise the T-Beam itself as the NTP server with DHCP option 42.
- In AP mode, local DNS can answer common public NTP hostnames with the T-Beam IP.
- Provides a captive portal for WiFi onboarding, AP settings, NTP alias settings, display settings, local time settings, and conservative Li-Ion power settings.
- Captive portal sections are collapsible and show caret indicators for collapsed/expanded state.
- Shows GPS, UTC, local time, Grid Square / Maidenhead locator, WiFi, and power status on the onboard OLED.
- Never transmits LoRa. Bluetooth is not used by the firmware.

## Current Build

- Firmware version: `v0.1.2`
- PlatformIO environment: `ttgo-t-beam`
- Upload port: `COM28`
- Filesystem: LittleFS, 1 MB at `0x300000`
- App partition: no OTA, about 3 MB
- Bluetooth: not used by the firmware
- LoRa: no LoRa library is initialized and no transmit path is used; AXP192 LDO2 is disabled by default

The workspace path contains a comma, so `platformio.ini` sets `build_dir` under `C:/Users/andy.baker/.platformio/build/tbeam_ntp_server` to avoid a Windows linker map-file issue.

## Flash

Use the local PlatformIO executable:

```powershell
$env:PYTHONIOENCODING='utf-8'
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -t uploadfs
```

`uploadfs` installs `data/settings.default.json`. Runtime settings are saved to `/settings.json` only when the portal Save action is used.

Binary releases include:

- `firmware.bin` for the application partition
- `littlefs.bin` containing factory defaults
- `partitions.csv` describing the flash layout

## First Boot

With no configured home WiFi, the device starts in AP mode:

- SSID: `TBeam-NTP-{mac}`
- Password: `tbeam-ntp`
- Default IP: `192.168.4.1`
- Portal: `http://192.168.4.1/`
- NTP: UDP/123 on the device IP
- DNS: UDP/53 on the device IP

The `{mac}` placeholder in the default AP SSID is expanded by the firmware to the last six hex digits of the unit MAC address, for example `TBeam-NTP-55B594`.

In AP mode, DHCP advertises the T-Beam as DNS and, when enabled, NTP server via DHCP option 42. DNS answers editable NTP aliases such as `time.cloudflare.com`, `time.google.com`, `time.nist.gov`, `pool.ntp.org`, and common OS time hosts with the T-Beam AP IP.

This is intentional. It reduces client configuration when there is no internet and users already have devices configured for common public time servers.

NTP intentionally fails closed: requests are counted but no NTP response is sent until recent valid GPS NMEA time is available.

## LAN Mode

The T-Beam can also join an existing WiFi network. In that mode it serves NTP on its assigned or static IPv4 address. Your router or clients can then be configured to use the T-Beam IP or hostname as an NTP source.

## Hardware Assumptions

- AXP192 PMU on I2C `SDA=21`, `SCL=22`, IRQ `GPIO35`
- GPS UART RX/TX: ESP32 RX `GPIO34`, TX `GPIO12`, 9600 baud
- GPS PPS input: `GPIO37`, rising edge
- User button: `GPIO38`
- OLED: SSD1306 at I2C `0x3C`

The firmware tolerates missing OLED, missing battery, and absent optional I2C sensors.

## Local Time And DST

UTC comes from GPS. Local display time can use an editable POSIX TZ rule string, which is the compact standard format supported by embedded C runtimes for timezone and daylight-saving rules.

The default rule is US Pacific time:

```text
PST8PDT,M3.2.0/2,M11.1.0/2
```

That means standard time is PST, daylight time is PDT, DST begins on the second Sunday in March at 02:00, and DST ends on the first Sunday in November at 02:00.

Examples:

- US Pacific: `PST8PDT,M3.2.0/2,M11.1.0/2`
- US Mountain without DST, such as most of Arizona: `MST7`
- UTC: `UTC0`

If the POSIX TZ/DST option is disabled, the firmware falls back to either the rough GPS-longitude offset or the manual offset in minutes.

## OLED And Button

The OLED cycles screens every 5 seconds by default. The cycle setting and screensaver timeout are configurable in the portal. Manual cycling pauses automatic cycling for 30 seconds; the screensaver timeout still applies normally.

Dedicated large-format screens are provided for UTC time, local time, and the Grid Square / Maidenhead locator. Other screens show GPS detail, network state, connected AP clients, and power status.

The user button wakes the screen. A short press cycles screens when the display is awake. Long AP SSID and AP password values scroll on the OLED network page.

Boot button behavior:

- Hold the user button during boot and release it before 10 seconds to force standalone AP mode for that boot.
- Hold it for 10 seconds at boot to request factory reset, then release and hold again for 10 seconds within the confirmation window to clear saved settings.

Runtime reset uses the same confirmation model: hold for 10 seconds to show the reset confirmation screen, then hold again for 10 seconds to reset.

## Power Defaults

Defaults are conservative for a single Li-Ion cell:

- Charging enabled
- Charge current: 280 mA
- Charge target: 4.10 V
- USB/VBUS input current limit: 500 mA
- Battery warning: 3.50 V while discharging
- Battery cutoff: 3.20 V while discharging, after a 30 second debounce
- AXP192 system power-down threshold: 3.00 V

Low-voltage shutdown is only requested when the PMU reports that a battery is present and discharging, so USB-only operation without a battery is expected to work.

## WiFi Client Capacity

The ESP32 IDF headers expose a larger SoftAP station table, while the Arduino `WiFi.softAP()` comments still describe 1-4 clients. This firmware exposes a UI range of 1-10 and defaults to 8.

For low-rate DHCP/DNS/NTP traffic, 8 clients is a reasonable starting point on this board. More may work, but the practical limit should be validated with real clients because WiFi management buffers, retries, RSSI, and captive-portal HTTP traffic matter more than NTP bandwidth.

## Known Limitations

- The POSIX TZ rule is editable, but the firmware does not carry a full IANA timezone database. That is deliberate: the ESP32 flash budget is better spent on the captive portal and field reliability.
- PPS discipline is implemented in firmware using GPS NMEA plus the PPS interrupt. It is suitable for this embedded NTP role, but it is not a full temperature-compensated oscillator PLL.
- DHCP option 42 is configured through the ESP32 lwIP DHCP server option hook. Some clients do not visibly report received option 42 even when they accept the DHCP lease.
