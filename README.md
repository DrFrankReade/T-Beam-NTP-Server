# T-Beam GPS Disciplined NTP Server


There are a great number of Lilygo / T-Beams out there from disappointing Meshtastic experiments. Now's a chance to give them new purpose. They've got a nice form factor, run on batteries, have screens, GPS antennas and can fit in pockets if you pull off the external antenna. 

This is firmware for the LilyGo/TTGO T-Beam, tested on T-Beam v1.1 hardware. It turns the board into a GPS/PPS disciplined standalone Stratum 1 NTP server. Provisional T-Beam v1.2 support is included, but v1.2 hardware has not been tested yet. Use v1.2 support at your own risk.

This project controls Li-Ion charging hardware, WiFi, GPS timing, and power-management rails. It is all use-at-your-own-risk firmware. Verify the hardware, battery chemistry, charging settings, and behavior before trusting it unattended.

The primary audience is the ham radio community and anyone else who needs accurate, stratum 1 local network time in remote, austere, or off-grid environments. The device can run as its own WiFi access point, DHCP server, DNS helper, captive portal, and NTP server, or it can join an existing LAN as a normal WiFi client.

NTP itself is always UTC. Local time and daylight-saving settings only affect the captive portal and OLED display. As such, they're completely safe to ignore.

## What It Does

- Serves NTP on UDP/123 from GPS time, disciplined by the GPS PPS input on `GPIO37`.

- Fails closed: if recent GPS time is not available, NTP requests are counted but no time is handed out.

- Runs standalone AP mode by default at `192.168.4.1`. Can be changed.

- In AP mode, DHCP can advertise the T-Beam itself as the NTP server with DHCP option 42. Not every client will listen.

- In AP mode, local DNS can answer common public NTP hostnames with the T-Beam IP. Option to respond to EVERY DNS request too (Useful?)

- Provides a captive portal for WiFi onboarding, AP settings, NTP alias settings, display settings, local time settings, and conservative Li-Ion power settings.

- Shows GPS, UTC, local time, Grid Square / Maidenhead locator, WiFi, and power status on the onboard OLED.

- Never transmits LoRa. OK to disconnect the antenna. Bluetooth is not used by the firmware.

## Current Build

- Firmware version: `v0.1.10`
- PlatformIO environment: `ttgo-t-beam`
- Upload port: `COM28`
- Filesystem: LittleFS, 1 MB at `0x300000`
- App partition: no OTA, about 3 MB
- Bluetooth: not used by the firmware
- LoRa: no LoRa library is initialized and no transmit path is used; the LoRa power rail is treated as unused

## Flash

### PlatformIO Development Flash

Use the local PlatformIO executable when building from source:

```powershell
$env:PYTHONIOENCODING='utf-8'
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -t uploadfs
```

`uploadfs` installs `data/index.html`, `data/settings.default.json`, and the timezone preset file. A PlatformIO pre-script generates `data/index.html.gz` from the readable portal source so browsers use less airtime. Runtime settings are saved to `/settings.json` only when the portal Save action is used.

### Combined Binary Release Flash

GitHub releases include a single combined image named like:

```text
t-beam-ntp-server-v0.1.10-combined.bin
```

This image includes the bootloader, partition table, boot app data, firmware, and LittleFS portal/settings defaults. Flashing it is intentionally simple and will erase the previous contents of the device, including saved settings.

Install the usual ESP32 flashing tool:

```powershell
py -m pip install esptool
```

Then flash the combined image. Replace `COM28` with your serial port:

```powershell
py -m esptool --chip esp32 --port COM28 --baud 460800 erase_flash
py -m esptool --chip esp32 --port COM28 --baud 460800 write_flash -z 0x0 t-beam-ntp-server-v0.1.10-combined.bin
```

If you already have PlatformIO installed, its bundled esptool also works:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m esptool --chip esp32 --port COM28 --baud 460800 erase_flash
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m esptool --chip esp32 --port COM28 --baud 460800 write_flash -z 0x0 t-beam-ntp-server-v0.1.10-combined.bin
```

After flashing, reset the board and join the default AP shown below.

## First Boot

By default, the device starts in explicit Standalone AP Mode:

- SSID: `TBeam-NTP-{mac}`
- Password: `tbeam-ntp`
- Security: WPA2-Personal
- Default IP: `192.168.4.1`
- Portal: `http://192.168.4.1/`
- NTP: UDP/123 on the device IP
- DNS: UDP/53 on the device IP

The `{mac}` placeholder in the default AP SSID is expanded by the firmware to the last six hex digits of the unit MAC address, for example `TBeam-NTP-55B594`.

In AP mode, DHCP advertises the T-Beam as DNS and, when enabled, NTP server via DHCP option 42. DNS answers editable NTP aliases such as `time.cloudflare.com`, `time.google.com`, `time.nist.gov`, `pool.ntp.org`, and common OS time hosts with the T-Beam AP IP.

This is intentional. It reduces client configuration when there is no internet and users already have devices configured for common public time servers.

NTP intentionally fails closed: requests are counted but no NTP response is sent until recent valid GPS NMEA time is available.

## LAN Mode

The T-Beam can also join an existing WiFi network by selecting Client Mode with AP Fallback in the portal and entering the Network Client SSID/password. In client mode it serves NTP on its assigned or static IPv4 address. Your router or clients can then be configured to use the T-Beam IP or hostname as an NTP source. DHCP and DNS servers are inactive, since it's assumed that the local router will be handling these services. If the client connection fails or is later lost, the device falls back to standalone AP mode.

Standalone AP security is configurable as Open, WPA2-Personal, WPA/WPA2 legacy, WPA2/WPA3 transition, or WPA3-Personal. WEP sucks and ESP32 SoftAP doesn't support it anyway.

## Hardware Support

- T-Beam v1.1 with AXP192 PMU is tested.
- T-Beam v1.2 with AXP2101 PMU has provisional support, but remains untested until v1.2 hardware is available. Use at your own risk.
- Hardware revision is detected automatically from the PMIC register `0x03`.
- The firmware does not care which LoRa radio is installed, or whether one is present.
- PMU I2C: `SDA=21`, `SCL=22`, IRQ `GPIO35`
- GPS UART RX/TX: ESP32 RX `GPIO34`, TX `GPIO12`, 9600 baud
- GPS PPS input: `GPIO37`, rising edge
- User button: `GPIO38`
- OLED: SSD1306 at I2C `0x3C`

The firmware tolerates missing OLED, missing battery, and absent optional I2C sensors.

## Local Time And DST

UTC comes from GPS. Local display time is display-only and does not affect NTP. The portal supports three local-time modes:

- Fixed UTC offset, entered as sign, hours, and minutes.
- Timezone preset, selected from `data/timezones.current.tsv`.
- Manual POSIX TZ rule string.

The timezone preset file is deliberately separate from the firmware logic. It is generated from IANA tzdata `2026c`, current as of July 9, 2026, and omits historical transitions. It can be edited, replaced, or removed; fixed-offset and manual POSIX modes still work without it.

The default timezone preset is `America/Los_Angeles`. The fallback/manual POSIX rule is US Pacific time, because Hollywood is the center of the universe. 

```text
PST8PDT,M3.2.0/2,M11.1.0/2
```

If you want to decode this, you'll see that it means standard time is PST, daylight time is PDT, DST begins on the second Sunday in March at 02:00, and DST ends on the first Sunday in November at 02:00.

Examples:

- US Pacific: `PST8PDT,M3.2.0/2,M11.1.0/2`
- US Mountain without DST, such as most of Arizona: `MST7`
- UTC: `UTC0`

The firmware does not carry a full historical IANA TZif database, because that would be pretty useless. Anyway, local time is for the OLED and portal, while NTP remains UTC.

## OLED And Button

The OLED cycles screens every 5 seconds by default.  There's a configurable screensaver timeout.

The user button wakes the screen. A short press cycles screens when the display is awake.

Factory Reset

- Hold the user button during boot and release it BEFORE 10 seconds to force standalone AP mode for that boot.
- Hold the user button for 10 seconds at boot to request factory reset, then release and hold again for 10 seconds within the confirmation window to clear saved settings.

## Power Defaults

**Do NOT use LiFePO4 batteries in any T-Beam.**

Defaults are conservative for a single Li-Ion cell:

- Charging enabled
- Charge current: 280 mA
- Charge target: 4.10 V
- USB/VBUS input current limit: 500 mA
- Battery warning: 3.50 V while discharging
- Battery cutoff: 3.20 V while discharging - When the system shuts down. 
- AXP192 system power-down threshold: 3.00 V - This is the voltage at which the battery is so dead that the system stops monitoring it and permanent battery damage is imminent. 

Low-voltage shutdown is only requested when the PMU reports that a battery is present and discharging, so USB-only operation without a battery works fine.

## WiFi Client Capacity

The ESP32 IDF headers expose a larger SoftAP station table, while the Arduino `WiFi.softAP()` comments still describe 1-4 clients. This firmware exposes a UI range of 1-10 and defaults to 8.

For low-rate DHCP/DNS/NTP traffic, 8 clients is a reasonable starting point on this board. More may work, but the practical limit should be validated with real clients because WiFi management buffers, retries, RSSI, and captive-portal HTTP traffic matter more than NTP bandwidth. If you notice problems or unstable behavior, it may be worth lowering this limit. 

## Known Limitations

- Timezone is entirely a cosmetic OLED "Nice to have" - The presets are current-as-of data, not a historical IANA TZ database. 
- PPS discipline is implemented in firmware using GPS NMEA plus the PPS interrupt. It is suitable for this embedded NTP role, but it is not a full temperature-compensated oscillator PLL. 
- It's only IPv4 - Is this really a problem?
- DHCP option 42 is configured through the ESP32 lwIP DHCP server option hook. Some clients don't visibly report received option 42 even when they accept the DHCP lease, and there's no guarantee that they're going to do anything with it if they do take it, hence the DNS wildcards. 
- T-Beam v1.2 support is provisional and untested. The firmware now detects AXP192 vs AXP2101 PMUs and maps the relevant rails, but only v1.1 hardware has been validated so far.
- Other T-Beam revisions remain unsupported unless their PMIC and rail mapping are explicitly verified. This matters because lithium batteries and power rails are involved.
- Power usage hasn't been optimized. 
