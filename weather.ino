/*
 * weather.ino — standalone sketch
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.8
 * Weather Dashboard — portrait/landscape toggle, open-meteo API.
 *
 * Controls:
 *   BOOT short press – toggle portrait / landscape
 *   PWR  short press – force refresh
 *
 * SD card: /setup/setup.txt  (see setup.txt in this folder for keys)
 */

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include "Arduino_GFX_Library.h"
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "XPowersLib.h"
#include "app_common.h"
#include "hw_panel.h"
#include "app_20_weather.h"


// ── Display ───────────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_OLED *gfx = nullptr;

// Shared canvas — allocated once so spi_bus_initialize() runs once.
Arduino_Canvas *g_canvas = nullptr;

// ── PMU ───────────────────────────────────────────────────────────────────────
XPowersPMU power;

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
  USBSerial.begin(115200);
  pinMode(0, INPUT_PULLUP);   // BOOT_BTN

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  if (!power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL))
    USBSerial.println("AXP2101 not found");
  power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  power.clearIrqStatus();
  power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  common_init();

  // Read brightness + timeout only; app_20 will re-mount SD for full config.
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (SD_MMC.begin("/sdcard", true)) {
    File f = SD_MMC.open("/setup/setup.txt");
    if (f) {
      char line[160];
      while (f.available()) {
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';
        const char *p;
        if ((p = strstr(line, "BRIGHTNESS")) != nullptr) {
          p += strlen("BRIGHTNESS");
          while (*p == ' ' || *p == '=') p++;
          int v = atoi(p);
          if (v > 0 && v <= 255) g_config.brightness = (uint16_t)v;
        }
        if ((p = strstr(line, "TIMEOUT")) != nullptr) {
          p += strlen("TIMEOUT");
          while (*p == ' ' || *p == '=') p++;
          int v = atoi(p);
          if (v >= 0) g_config.timeout_s = (uint32_t)v;
        }
      }
      f.close();
    }
    SD_MMC.end();   // app_20 re-mounts to read full config (SSID, LOCATION, etc.)
  }

  gfx = make_display(bus);
  g_canvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, gfx, 0, 0, 1);
  if (!g_canvas->begin()) USBSerial.println("g_canvas->begin() failed");
  gfx->setBrightness(g_config.brightness);

  app20_setup(gfx);
}

void loop() {
  app20_loop();
}
