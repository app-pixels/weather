# weather

**Weather** · v1.0.0

Live weather and forecast for any location, fetched over WiFi.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#tool` `#wifi`

Pulls current conditions and a multi-day forecast over WiFi and renders them on the AMOLED.

## Controls
- **BOOT** — cycle through configured locations
- **PWR** — refresh now

## `setup.txt` keys
**Mandatory**
- `SSID` / `PASSWORD` — WiFi
- `LOCATION_1` — city name, e.g. `Vienna`

**Optional**
- `SSID2` / `PASSWORD2`, `SSID3` / `PASSWORD3` — WiFi fallbacks
- `LOCATION_2`, `LOCATION_3` — extra cities (cycle with **PWR**)
- `WEATHER_API` — provider id (`open-meteo` default, no key needed)
- `WEATHER_API_KEY` — required only for paid providers (`open-weather`)
- `WEATHER_REFRESH_MIN` — refresh interval in minutes (default `10`)
- `TIMEZONE` — POSIX TZ string

## Editing `setup.txt`
The device reads `/setup/setup.txt` from the SD card on boot. [Download a working sample](https://sosbxffigpteqilpgxwn.supabase.co/storage/v1/object/public/app-assets/setup/setup.txt) — covers every app — and edit the keys you need.

Don't want to eject the card? Use the [**USB Stick**](/apps/usb-stick) app (mounts the SD card as a USB drive over USB-C) or the [**Filehub**](/apps/filehub) app (edit over WiFi).

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - Adafruit XCA9554
   - ArduinoJson (bblanchon)
   - GFX Library for Arduino (moononournation)
   - XPowersLib (lewishe)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/weather_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/weather_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/weather).

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/weather
