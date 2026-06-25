/*
 * app_20_weather.cpp — WeatherDashboard v2
 *
 * Changes from v1:
 *   - Up to 3 WiFi SSIDs with fallback (SSID/SSID2/SSID3, PASSWORD/PASSWORD2/PASSWORD3)
 *   - Switched to open-meteo API (no key, auto-timezone, km/h wind)
 *   - 8-slot × 6h forecast = 48 h
 *   - Precipitation always shown (even 0 mm)
 *   - Wind in km/h
 *   - Correct local time via NTP + UTC offset from API
 *   - BOOT: toggle portrait (368×448) / landscape (448×368)
 *   - PWR short press: force refresh
 *   - True black background everywhere (0x0000)
 */

#include "app_20_weather.h"
#include "app_common.h"
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <FS.h>
#include <time.h>
#include <ArduinoJson.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include <Adafruit_XCA9554.h>

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;

static Arduino_Canvas   *canvas  = nullptr;
static Adafruit_XCA9554  expander;

// ── Config ───────────────────────────────────────────────────────────────────
static char cfg_ssid[3][64] = {};
static char cfg_pass[3][64] = {};
static char cfg_locations[3][64] = {};
static int  cfg_nLocations = 0;
static char cfg_api[16]    = "open-meteo";
static char cfg_apiKey[64] = "";
static int  cfg_refreshMin = 10;

// ── Rotation: 1 = landscape 448×368 (default), 0 = portrait 368×448 ──────────
static uint8_t s_rot = 1;
#define DW (s_rot ? 448 : 368)
#define DH (s_rot ? 368 : 448)

// ── Colors (true black) ───────────────────────────────────────────────────────
#define COL_BG      0x0000
#define COL_WHITE   0xFFFF
#define COL_LGREY   0xC618
#define COL_GREY    0x7BEF
#define COL_DGREY   0x2104
#define COL_YELLOW  0xFFE0
#define COL_ORANGE  0xFD20
#define COL_LBLUE   0x867F
#define COL_CYAN    0x07FF
#define COL_DIVIDER 0x2945

// ── Data ─────────────────────────────────────────────────────────────────────
static float s_lat[3] = {0}, s_lon[3] = {0};
static bool  s_geocoded[3] = {false, false, false};
static int   s_locIdx = 0;

// 3-day forecast: 3 days × 3 fixed time slots (08h, 14h, 20h)
static const int   SLOT_HOURS[3]  = {8, 14, 20};
static const char *SLOT_LABELS[3] = {"08", "14", "20"};
static const char *WDAY_NAMES[]   = {"SUNDAY","MONDAY","TUESDAY",
    "WEDNESDAY","THURSDAY","FRIDAY","SATURDAY"};

struct WeatherData {
    float temp, windSpeed, windDir, humidity, precip;
    float precipHour;     // precipitation next 1 h
    int   wmo;
    // Forecast grid [day 0-2][slot 0-2 = 08/14/20h]
    int   dayWday[3];     // tm_wday index for each day
    float fTemp[3][3];
    float fPrecip[3][3];
    int   fWmo[3][3];
    bool  valid;
};
static WeatherData wx = {};

static uint32_t lastFetch = 0;
static uint32_t lastDraw  = 0;
static uint32_t s_fetchMs = 10UL * 60UL * 1000UL;
#define DRAW_MS   (60UL * 1000UL)          // redraw for clock update

// ── Buttons ───────────────────────────────────────────────────────────────────
#define BOOT_BTN     0
#define PWR_POLL_MS  50
#define BOOT_LONG_MS 800
static bool     bootWas        = false;
static uint32_t bootDownAt     = 0;
static bool     bootLongFired  = false;
static uint32_t lastPwrPoll    = 0;

// ── Config parser ─────────────────────────────────────────────────────────────
// Matches KEY only if the char immediately after is not alphanumeric or '_'
static bool extractVal(const char *line, const char *key, char *out, size_t cap) {
    // Anchored at line start so "SSID" doesn't match inside "AP_SSID".
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    size_t kl = strlen(key);
    if (strncmp(p, key, kl) != 0) return false;
    char after = p[kl];
    if (isalnum((unsigned char)after) || after == '_') return false;
    p += kl;
    while (*p == ' ' || *p == '=') p++;
    if (*p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && *p != '\n' && *p != '\r' && n < cap - 1)
        out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
}

static bool readConfig() {
    File f = SD_MMC.open("/setup/setup.txt");
    if (!f) return false;
    char line[160];
    char loc1[64] = {}, loc2[64] = {}, loc3[64] = {}, locOld[64] = {};
    char refreshBuf[16] = {};
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        extractVal(line, "SSID",       cfg_ssid[0], 64);
        extractVal(line, "PASSWORD",   cfg_pass[0], 64);
        extractVal(line, "SSID2",      cfg_ssid[1], 64);
        extractVal(line, "PASSWORD2",  cfg_pass[1], 64);
        extractVal(line, "SSID3",      cfg_ssid[2], 64);
        extractVal(line, "PASSWORD3",  cfg_pass[2], 64);
        extractVal(line, "LOCATION_1", loc1, 64);
        extractVal(line, "LOCATION_2", loc2, 64);
        extractVal(line, "LOCATION_3", loc3, 64);
        extractVal(line, "LOCATION",   locOld, 64);   // legacy single-city
        extractVal(line, "WEATHER_API_KEY",     cfg_apiKey,  64);
        extractVal(line, "WEATHER_API",         cfg_api,     16);
        extractVal(line, "WEATHER_REFRESH_MIN", refreshBuf,  16);
    }
    f.close();

    cfg_nLocations = 0;
    if (loc1[0]) { strncpy(cfg_locations[cfg_nLocations++], loc1, 64); }
    if (loc2[0]) { strncpy(cfg_locations[cfg_nLocations++], loc2, 64); }
    if (loc3[0]) { strncpy(cfg_locations[cfg_nLocations++], loc3, 64); }
    if (cfg_nLocations == 0 && locOld[0]) {
        strncpy(cfg_locations[cfg_nLocations++], locOld, 64);
    }
    if (refreshBuf[0]) {
        int v = atoi(refreshBuf);
        if (v >= 1 && v <= 1440) s_fetchMs = (uint32_t)v * 60UL * 1000UL;
    }
    return cfg_ssid[0][0] != '\0' && cfg_nLocations > 0;
}

// ── Status splash ─────────────────────────────────────────────────────────────
static void showStatus(const char *l1, const char *l2 = nullptr) {
    canvas->setRotation(s_rot);
    canvas->fillScreen(COL_BG);
    canvas->setTextColor(COL_WHITE); canvas->setTextSize(2);
    int16_t y = DH / 2 - (l2 ? 16 : 8);
    canvas->setCursor(16, y); canvas->print(l1);
    if (l2) {
        canvas->setTextColor(COL_GREY); canvas->setTextSize(2);
        canvas->setCursor(16, y + 28); canvas->print(l2);
    }
    canvas->flush();
}

// ── WiFi — try up to 3 SSIDs ─────────────────────────────────────────────────
static bool wifiConnect() {
    showStatus("Connecting WiFi...",
               cfg_ssid[0][0] ? cfg_ssid[0] : "");
    WifiCred list[3] = {
        { cfg_ssid[0], cfg_pass[0] },
        { cfg_ssid[1], cfg_pass[1] },
        { cfg_ssid[2], cfg_pass[2] },
    };
    return wifi_try_connect(list, 3) >= 0;
}

// symbolToWmo removed — open-meteo returns WMO codes directly

// ── Geocoding (open-meteo, used by all backends) ─────────────────────────────
static bool geocodeCity(int idx) {
    if (idx < 0 || idx >= cfg_nLocations) return false;
    if (s_geocoded[idx]) return true;       // cached

    char enc[128] = {}, url[256];
    int j = 0;
    const char *loc = cfg_locations[idx];
    for (int i = 0; loc[i] && j < 120; i++) {
        if (loc[i] == ' ') { enc[j++]='%'; enc[j++]='2'; enc[j++]='0'; }
        else enc[j++] = loc[i];
    }
    snprintf(url, sizeof(url),
        "https://geocoding-api.open-meteo.com/v1/search"
        "?name=%s&count=1&language=en&format=json", enc);
    WiFiClientSecure cli; cli.setInsecure();
    HTTPClient http; http.begin(cli, url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    JsonDocument doc;
    deserializeJson(doc, http.getStream());
    http.end();
    JsonArray r = doc["results"];
    if (r.isNull() || r.size() == 0) return false;
    s_lat[idx] = r[0]["latitude"].as<float>();
    s_lon[idx] = r[0]["longitude"].as<float>();
    s_geocoded[idx] = true;
    USBSerial.printf("Coords[%d]: %.4f, %.4f\n", idx, s_lat[idx], s_lon[idx]);
    return true;
}

// ── Weather fetch (open-meteo, 3-day forecast, km/h) ─────────────────────────
// open-meteo is the only fully-implemented backend right now. met.no and
// open-weather are recognised in setup.txt but currently fall through to
// open-meteo with a console warning. Their full plumbing (different JSON
// shapes, User-Agent for met.no, key for open-weather) is staged for a
// follow-up.
static bool fetchWeather() {
    if (cfg_nLocations == 0) return false;
    int idx = s_locIdx;
    float lat = s_lat[idx], lon = s_lon[idx];

    if (strcmp(cfg_api, "met.no") == 0 || strcmp(cfg_api, "open-weather") == 0) {
        USBSerial.printf("WEATHER_API=%s not yet wired up — using open-meteo\n", cfg_api);
    }

    // Initial NTP sync with longitude-estimated offset
    int32_t estOfs = (int32_t)(lon / 15.0f * 3600.0f);
    configTime(estOfs, 0, "pool.ntp.org", "time.nist.gov");
    delay(1000);

    char url[400];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,"
        "wind_direction_10m,precipitation,weather_code"
        "&hourly=temperature_2m,precipitation,weather_code"
        "&forecast_days=3&timezone=auto&wind_speed_unit=kmh",
        lat, lon);

    WiFiClientSecure cli; cli.setInsecure();
    HTTPClient http;
    http.begin(cli, url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) {
        USBSerial.printf("open-meteo HTTP %d\n", code);
        char errmsg[32]; snprintf(errmsg, sizeof(errmsg), "HTTP error %d", code);
        showStatus("Fetch failed!", errmsg);
        http.end(); return false;
    }

    String body = http.getString();
    http.end();
    if (body.length() == 0) { showStatus("Fetch failed!", "empty response"); return false; }
    USBSerial.printf("Body len: %d\n", body.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        USBSerial.printf("JSON: %s\n", err.c_str());
        showStatus("JSON error!", err.c_str());
        return false;
    }

    JsonObject cur = doc["current"];
    if (cur.isNull()) { showStatus("Fetch failed!", "no 'current' in JSON"); return false; }
    wx.temp      = cur["temperature_2m"].as<float>();
    wx.humidity  = cur["relative_humidity_2m"].as<float>();
    wx.windSpeed = cur["wind_speed_10m"].as<float>();   // already km/h
    wx.windDir   = cur["wind_direction_10m"].as<float>();
    wx.precip    = cur["precipitation"].as<float>();
    wx.wmo       = cur["weather_code"].as<int>();

    // Re-sync NTP with exact offset from API response
    int32_t utcOfs = doc["utc_offset_seconds"].as<int32_t>();
    configTime(utcOfs, 0, "pool.ntp.org", "time.nist.gov");
    delay(400);

    JsonArray times   = doc["hourly"]["time"];
    JsonArray hTemps  = doc["hourly"]["temperature_2m"];
    JsonArray hPrecip = doc["hourly"]["precipitation"];
    JsonArray hCodes  = doc["hourly"]["weather_code"];
    if (hTemps.isNull() || hTemps.size() == 0) {
        showStatus("Fetch failed!", "no hourly data");
        return false;
    }

    // Derive today's weekday from API date (more reliable than NTP timing)
    int todayWday = 0;
    if (times.size() > 0) {
        struct tm dayTm = {};
        const char *t0 = times[0] | "";
        sscanf(t0, "%d-%d-%d", &dayTm.tm_year, &dayTm.tm_mon, &dayTm.tm_mday);
        dayTm.tm_year -= 1900;
        dayTm.tm_mon  -= 1;
        mktime(&dayTm);
        todayWday = dayTm.tm_wday;
    }

    // Get current hour for next-1h precip
    struct tm ti;
    int curHour = 0;
    if (getLocalTime(&ti, 300)) curHour = ti.tm_hour;

    // Precipitation next 1 h = hourly precip at curHour + 1
    int nextIdx = curHour + 1;
    wx.precipHour = ((size_t)nextIdx < hPrecip.size()) ? hPrecip[nextIdx].as<float>() : 0.0f;

    // Hourly array: index 0 = midnight today, so day d hour h = d*24 + h
    for (int d = 0; d < 3; d++) {
        wx.dayWday[d] = (todayWday + d) % 7;
        for (int s = 0; s < 3; s++) {
            int idx = d * 24 + SLOT_HOURS[s];
            if ((size_t)idx < hTemps.size()) {
                wx.fTemp[d][s]   = hTemps[idx].as<float>();
                wx.fPrecip[d][s] = hPrecip[idx].as<float>();
                wx.fWmo[d][s]    = hCodes[idx].as<int>();
            } else {
                wx.fTemp[d][s] = wx.fPrecip[d][s] = 0.0f;
                wx.fWmo[d][s]  = 0;
            }
        }
    }

    wx.valid = true;
    USBSerial.printf("open-meteo OK: %.1f C  wmo=%d  wind=%.1f km/h\n",
                     wx.temp, wx.wmo, wx.windSpeed);
    return true;
}

// ── WMO helpers ───────────────────────────────────────────────────────────────
static const char *wmoText(int c) {
    if (c == 0)          return "Clear sky";
    if (c <= 2)          return "Mainly clear";
    if (c <= 3)          return "Overcast";
    if (c <= 48)         return "Fog";
    if (c <= 55)         return "Drizzle";
    if (c <= 65)         return "Rain";
    if (c <= 77)         return "Snow";
    if (c <= 82)         return "Showers";
    if (c <= 86)         return "Snow showers";
    if (c == 95)         return "Thunderstorm";
    return "T-storm+hail";
}

static uint16_t wmoColor(int c) {
    if (c <= 1)  return COL_YELLOW;
    if (c <= 3)  return COL_LGREY;
    if (c <= 48) return COL_GREY;
    if (c <= 65) return COL_LBLUE;
    if (c <= 77) return COL_CYAN;
    if (c <= 82) return COL_LBLUE;
    if (c <= 86) return COL_CYAN;
    return COL_ORANGE;
}

static const char *windDirStr(float d) {
    const char *s[] = {"N","NE","E","SE","S","SW","W","NW"};
    return s[((int)((d + 22.5f) / 45.0f)) & 7];
}

// ── Icon ──────────────────────────────────────────────────────────────────────
static void drawIcon(int16_t cx, int16_t cy, int16_t sz, int wmo) {
    int16_t h = sz / 2;
    if (wmo <= 1) {
        canvas->fillCircle(cx, cy, sz * 7 / 10, COL_YELLOW);
    } else if (wmo == 2) {
        canvas->fillCircle(cx - h/2, cy - h/2, h*3/2,  COL_YELLOW);
        canvas->fillCircle(cx - h/2, cy + h/2, h,      COL_LGREY);
        canvas->fillCircle(cx,       cy + h/4, h,       COL_LGREY);
        canvas->fillCircle(cx + h/2, cy + h/2, h*3/4,  COL_LGREY);
        canvas->fillRect(cx - h/2, cy + h/2, h + h/2, h, COL_LGREY);
    } else if (wmo == 3 || (wmo >= 45 && wmo <= 48)) {
        canvas->fillCircle(cx - h, cy,     h,      COL_LGREY);
        canvas->fillCircle(cx,     cy-h/2, h*5/4,  COL_LGREY);
        canvas->fillCircle(cx + h, cy,     h,      COL_LGREY);
        canvas->fillRect(cx - h, cy, h * 2, h, COL_LGREY);
    } else if (wmo >= 71 && wmo <= 77) {
        canvas->fillCircle(cx - h, cy,     h,     COL_LGREY);
        canvas->fillCircle(cx,     cy-h/2, h*5/4, COL_LGREY);
        canvas->fillCircle(cx + h, cy,     h,     COL_LGREY);
        canvas->fillRect(cx - h, cy, h * 2, h, COL_LGREY);
        for (int i = -1; i <= 1; i++)
            canvas->fillCircle(cx + i*(sz/3), cy + sz*3/4, 3, 0xFFFF);
    } else if (wmo >= 95) {
        canvas->fillCircle(cx - h, cy,     h,     COL_DGREY);
        canvas->fillCircle(cx,     cy-h/2, h*5/4, COL_DGREY);
        canvas->fillCircle(cx + h, cy,     h,     COL_DGREY);
        canvas->fillRect(cx - h, cy, h * 2, h, COL_DGREY);
        canvas->drawLine(cx+4, cy+h/2,    cx-4, cy+sz*3/4, COL_YELLOW);
        canvas->drawLine(cx-4, cy+sz*3/4, cx+4, cy+sz,     COL_YELLOW);
    } else {
        // rain, drizzle, showers (wmo 51–65, 80–86)
        uint16_t cc = (wmo >= 85) ? COL_CYAN : COL_LBLUE;
        canvas->fillCircle(cx - h, cy,     h,     COL_GREY);
        canvas->fillCircle(cx,     cy-h/2, h*5/4, COL_GREY);
        canvas->fillCircle(cx + h, cy,     h,     COL_GREY);
        canvas->fillRect(cx - h, cy, h * 2, h, COL_GREY);
        for (int i = -1; i <= 1; i++)
            canvas->drawLine(cx+i*(sz/3), cy+h, cx+i*(sz/3)-4, cy+sz, cc);
    }
}

// ── Local time helper ─────────────────────────────────────────────────────────
static void nowStr(char *buf, size_t cap) {
    struct tm ti;
    if (getLocalTime(&ti, 100))
        snprintf(buf, cap, "%02d:%02d", ti.tm_hour, ti.tm_min);
    else
        strncpy(buf, "--:--", cap);
}

// ── Draw — landscape 448×368 ──────────────────────────────────────────────────
static void drawLandscape() {
    const int16_t W = 448, H = 368;
    // HDR_H = 64: pill row (y 4-28) + 8px gap + divider y=36 + 8px gap + city/time row (y 44-60).
    const int16_t HDR_H  = 64;
    const int16_t CUR_H  = 150;
    const int16_t DIV_Y  = HDR_H + CUR_H;
    const int16_t FCST_Y = DIV_Y + 3;
    const int16_t FCST_H = H - FCST_Y;     // ~161 px
    const int16_t DAY_W  = W / 3;          // ~149 px per day column
    const int16_t SUB_W  = DAY_W / 3;      // ~49 px per time slot

    canvas->fillScreen(COL_BG);

    // Header: city (left) | time (right). Battery at top-right corner.
    // Pills sit at y=4-28; give them breathing room before the divider, then
    // a touch more before the city/time row.
    canvas->drawFastHLine(0, 36, W, COL_DIVIDER);
    canvas->setTextSize(2); canvas->setTextColor(COL_WHITE);
    const char *city = cfg_locations[s_locIdx];
    canvas->setCursor(24, 44); canvas->print(city);

    char tbuf[8]; nowStr(tbuf, sizeof(tbuf));
    canvas->setTextSize(2); canvas->setTextColor(COL_GREY);
    int16_t tw = (int16_t)(strlen(tbuf) * 12);
    canvas->setCursor(W - 24 - tw - 52, 44); canvas->print(tbuf);

    draw_battery_g(canvas, W, H);
    draw_watermark_g(canvas, W, H);
    draw_pill_label(canvas, 1, 0, "rot");
    draw_pill_label(canvas, 1, 1, "fetch");

    // Current — icon | temp | stats
    int16_t iCx = 58, iCy = HDR_H + CUR_H / 2 - 10;
    drawIcon(iCx, iCy, 40, wx.wmo);
    canvas->setTextSize(2); canvas->setTextColor(wmoColor(wx.wmo));
    const char *cond = wmoText(wx.wmo);
    int16_t cwx = iCx - (int16_t)(strlen(cond) * 6);
    if (cwx < 4) cwx = 4;
    canvas->setCursor(cwx, iCy + 46);
    canvas->print(cond);

    // Temperature — large
    char tStr[8]; snprintf(tStr, sizeof(tStr), "%.1f", wx.temp);
    canvas->setTextSize(4, 5, 1); canvas->setTextColor(wmoColor(wx.wmo));
    tw = (int16_t)(strlen(tStr) * 25);
    int16_t tx = 170 - tw / 2;
    canvas->setCursor(tx, HDR_H + 20); canvas->print(tStr);
    // Degree symbol + C
    canvas->drawCircle(tx + tw + 4, HDR_H + 22, 3, COL_LGREY);
    canvas->setTextSize(2); canvas->setTextColor(COL_LGREY);
    canvas->setCursor(tx + tw + 12, HDR_H + 20); canvas->print("C");

    // Right stats panel — labels size 2 (body) per style guide.
    char buf[32];
    int16_t rx = 280;
    // Wind
    canvas->setTextSize(2); canvas->setTextColor(COL_GREY);
    canvas->setCursor(rx, HDR_H + 8); canvas->print("Wind");
    canvas->setTextSize(2); canvas->setTextColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "%.0f km/h %s", wx.windSpeed, windDirStr(wx.windDir));
    canvas->setCursor(rx, HDR_H + 28); canvas->print(buf);

    // Humidity
    canvas->setTextSize(2); canvas->setTextColor(COL_GREY);
    canvas->setCursor(rx, HDR_H + 56); canvas->print("Humidity");
    canvas->setTextSize(2); canvas->setTextColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "%.0f %%", wx.humidity);
    canvas->setCursor(rx, HDR_H + 76); canvas->print(buf);

    // Precip next 1 h
    canvas->setTextSize(2); canvas->setTextColor(COL_GREY);
    canvas->setCursor(rx, HDR_H + 104); canvas->print("Precip 1h");
    canvas->setTextSize(2); canvas->setTextColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "%.0f mm", wx.precipHour);
    canvas->setCursor(rx, HDR_H + 124); canvas->print(buf);

    // Divider
    canvas->drawFastHLine(0, DIV_Y,     W, COL_DIVIDER);
    canvas->drawFastHLine(0, DIV_Y + 1, W, COL_DIVIDER);

    // Forecast: 3 day columns, each with 3 time slots (08, 14, 20)
    // Vertical dividers stop short of the watermark zone.
    const int16_t VDIV_LEN = FCST_H - 26;
    for (int d = 0; d < 3; d++) {
        int16_t dx = d * DAY_W;   // left edge of day column
        if (d > 0) canvas->drawFastVLine(dx, FCST_Y, VDIV_LEN, COL_DIVIDER);

        // Day name heading
        const char *dn = WDAY_NAMES[wx.dayWday[d]];
        canvas->setTextSize(2); canvas->setTextColor(d == 0 ? COL_WHITE : COL_GREY);
        tw = (int16_t)(strlen(dn) * 12);
        canvas->setCursor(dx + (DAY_W - tw) / 2, FCST_Y + 4);
        canvas->print(dn);

        // Thin line under day name
        canvas->drawFastHLine(dx + 4, FCST_Y + 22, DAY_W - 8, COL_DIVIDER);

        // 3 sub-columns: 08, 14, 20
        for (int s = 0; s < 3; s++) {
            int16_t sx = dx + s * SUB_W + SUB_W / 2;  // center of sub-column

            // Hour label
            canvas->setTextSize(2); canvas->setTextColor(COL_GREY);
            tw = (int16_t)(strlen(SLOT_LABELS[s]) * 12);
            canvas->setCursor(sx - tw/2, FCST_Y + 28);
            canvas->print(SLOT_LABELS[s]);

            // Weather icon (size 20, bumped from 16 for visibility)
            drawIcon(sx, FCST_Y + 62, 20, wx.fWmo[d][s]);

            // Temperature + degree symbol (top-anchored: ~10 px below icon)
            snprintf(buf, sizeof(buf), "%.0f", wx.fTemp[d][s]);
            canvas->setTextSize(2); canvas->setTextColor(COL_WHITE);
            tw = (int16_t)(strlen(buf) * 12);
            canvas->setCursor(sx - tw/2, FCST_Y + 86);
            canvas->print(buf);
            canvas->drawCircle(sx + tw/2 + 3, FCST_Y + 88, 2, COL_GREY);

            // Precip (integer, no decimal)
            snprintf(buf, sizeof(buf), "%.0f", wx.fPrecip[d][s]);
            canvas->setTextSize(2);
            canvas->setTextColor(wx.fPrecip[d][s] >= 0.1f ? COL_LBLUE : COL_DGREY);
            tw = (int16_t)(strlen(buf) * 12);
            canvas->setCursor(sx - tw/2, FCST_Y + 110);
            canvas->print(buf);
        }
    }

    // Hairline between precip values and watermark
    canvas->drawFastHLine(0, H - 22, W, COL_DIVIDER);

    canvas->flush();
}

// ── Draw — portrait 368×448 ───────────────────────────────────────────────────
static void drawPortrait() {
    const int16_t W = 368, H = 448;
    const int16_t HDR_H  = 52;   // divider y=14 + 8px gap + city y=22..38 + 14px breathing
    const int16_t CUR_H  = 158;
    const int16_t DIV_Y  = HDR_H + CUR_H;
    const int16_t FCST_Y = DIV_Y + 3;
    const int16_t FCST_H = H - FCST_Y;    // ~243 px
    const int16_t DAY_W  = W / 3;         // ~122 px per day column
    const int16_t SUB_W  = DAY_W / 3;     // ~40 px per time slot

    canvas->fillScreen(COL_BG);

    // Header: city left, time right, battery top-right
    // A bit of breathing room before the divider, then again before the
    // city/time row.
    canvas->drawFastHLine(0, 14, W, COL_DIVIDER);
    canvas->setTextSize(2); canvas->setTextColor(COL_WHITE);
    const char *city = cfg_locations[s_locIdx];
    int16_t tw = (int16_t)(strlen(city) * 12);
    canvas->setCursor(24, 22); canvas->print(city);

    char tbuf[8]; nowStr(tbuf, sizeof(tbuf));
    canvas->setTextSize(2); canvas->setTextColor(COL_GREY);
    tw = (int16_t)(strlen(tbuf) * 12);
    canvas->setCursor(W - 24 - tw - 52, 22); canvas->print(tbuf);

    draw_battery_g(canvas, W, H);
    draw_watermark_g(canvas, W, H);
    draw_pill_label(canvas, 0, 0, "rot");
    draw_pill_label(canvas, 0, 1, "fetch");

    // Current: icon left | big temp centre | stats right
    drawIcon(46, HDR_H + 48, 38, wx.wmo);
    canvas->setTextSize(2); canvas->setTextColor(wmoColor(wx.wmo));
    const char *cond = wmoText(wx.wmo);
    int16_t cwx = 46 - (int16_t)(strlen(cond) * 6);
    if (cwx < 4) cwx = 4;
    canvas->setCursor(cwx, HDR_H + 90);
    canvas->print(cond);

    char tStr[8]; snprintf(tStr, sizeof(tStr), "%.1f", wx.temp);
    canvas->setTextSize(4, 5, 1); canvas->setTextColor(wmoColor(wx.wmo));
    tw = (int16_t)(strlen(tStr) * 25);
    int16_t tx = W / 2 - tw / 2;
    canvas->setCursor(tx, HDR_H + 14); canvas->print(tStr);
    // Degree symbol + C
    canvas->drawCircle(tx + tw + 4, HDR_H + 16, 3, COL_LGREY);
    canvas->setTextSize(2); canvas->setTextColor(COL_LGREY);
    canvas->setCursor(tx + tw + 12, HDR_H + 14); canvas->print("C");

    // Stats row below hero, 3 columns (Wind | Humidity | Precip 1h).
    // Right-column stacking would overlap the hero temp at this text size.
    char buf[32];
    const int16_t CW = W / 3;
    const int16_t CX0 = CW / 2;
    const int16_t CX1 = CW + CW / 2;
    const int16_t CX2 = 2 * CW + CW / 2;
    const int16_t LBL_Y = HDR_H + 114;
    const int16_t VAL_Y = HDR_H + 134;

    // Labels
    canvas->setTextSize(2); canvas->setTextColor(COL_GREY);
    tw = 4 * 12;
    canvas->setCursor(CX0 - tw/2, LBL_Y); canvas->print("Wind");
    tw = 8 * 12;
    canvas->setCursor(CX1 - tw/2, LBL_Y); canvas->print("Humidity");
    tw = 9 * 12;
    canvas->setCursor(CX2 - tw/2, LBL_Y); canvas->print("Precip 1h");

    // Values
    canvas->setTextSize(2); canvas->setTextColor(COL_WHITE);
    snprintf(buf, sizeof(buf), "%.0f km/h %s", wx.windSpeed, windDirStr(wx.windDir));
    tw = (int16_t)(strlen(buf) * 12);
    canvas->setCursor(CX0 - tw/2, VAL_Y); canvas->print(buf);

    snprintf(buf, sizeof(buf), "%.0f %%", wx.humidity);
    tw = (int16_t)(strlen(buf) * 12);
    canvas->setCursor(CX1 - tw/2, VAL_Y); canvas->print(buf);

    snprintf(buf, sizeof(buf), "%.0f mm", wx.precipHour);
    tw = (int16_t)(strlen(buf) * 12);
    canvas->setCursor(CX2 - tw/2, VAL_Y); canvas->print(buf);

    // Divider
    canvas->drawFastHLine(0, DIV_Y,     W, COL_DIVIDER);
    canvas->drawFastHLine(0, DIV_Y + 1, W, COL_DIVIDER);

    // Forecast: 3 day columns, each with 3 time slots (08, 14, 20)
    // Portrait has lots of vertical room — spread the rows so the bottom
    // doesn't feel sparse. Vertical dividers stop short of the watermark.
    const int16_t VDIV_LEN = FCST_H - 26;
    for (int d = 0; d < 3; d++) {
        int16_t dx = d * DAY_W;
        if (d > 0) canvas->drawFastVLine(dx, FCST_Y, VDIV_LEN, COL_DIVIDER);

        // Day name heading — use short 3-letter name (portrait is narrower)
        const char *dn = WDAY_NAMES[wx.dayWday[d]];
        char dn3[4]; strncpy(dn3, dn, 3); dn3[3] = '\0';
        canvas->setTextSize(2); canvas->setTextColor(d == 0 ? COL_WHITE : COL_GREY);
        tw = (int16_t)(strlen(dn3) * 12);
        canvas->setCursor(dx + (DAY_W - tw) / 2, FCST_Y + 8);
        canvas->print(dn3);

        canvas->drawFastHLine(dx + 4, FCST_Y + 30, DAY_W - 8, COL_DIVIDER);

        // 3 sub-columns: 08, 14, 20
        for (int s = 0; s < 3; s++) {
            int16_t sx = dx + s * SUB_W + SUB_W / 2;

            // Hour label
            canvas->setTextSize(2); canvas->setTextColor(COL_GREY);
            tw = (int16_t)(strlen(SLOT_LABELS[s]) * 12);
            canvas->setCursor(sx - tw/2, FCST_Y + 38);
            canvas->print(SLOT_LABELS[s]);

            // Weather icon
            drawIcon(sx, FCST_Y + 88, 20, wx.fWmo[d][s]);

            // Temperature + degree symbol
            snprintf(buf, sizeof(buf), "%.0f", wx.fTemp[d][s]);
            canvas->setTextSize(2); canvas->setTextColor(COL_WHITE);
            tw = (int16_t)(strlen(buf) * 12);
            canvas->setCursor(sx - tw/2, FCST_Y + 134);
            canvas->print(buf);
            canvas->drawCircle(sx + tw/2 + 3, FCST_Y + 136, 2, COL_GREY);

            // Precip (integer, no decimal)
            snprintf(buf, sizeof(buf), "%.0f", wx.fPrecip[d][s]);
            canvas->setTextSize(2);
            canvas->setTextColor(wx.fPrecip[d][s] >= 0.1f ? COL_LBLUE : COL_DGREY);
            tw = (int16_t)(strlen(buf) * 12);
            canvas->setCursor(sx - tw/2, FCST_Y + 178);
            canvas->print(buf);
        }
    }

    // Hairline between precip values and watermark
    canvas->drawFastHLine(0, H - 22, W, COL_DIVIDER);

    canvas->flush();
}

static void drawDashboard() {
    canvas->setRotation(s_rot);
    if (s_rot) drawLandscape();
    else        drawPortrait();
    lastDraw = millis();
}

// ── App entry points ──────────────────────────────────────────────────────────
void app20_setup(Arduino_OLED * /*passed_gfx*/) {
    if (!expander.begin(0x20)) USBSerial.println("XCA9554 init failed");
    expander.pinMode(1, OUTPUT); expander.digitalWrite(1, LOW);
    expander.pinMode(2, OUTPUT); expander.digitalWrite(2, LOW);
    delay(20);
    expander.digitalWrite(1, HIGH);
    expander.digitalWrite(2, HIGH);

    canvas = g_canvas;
    s_rot  = 1;
    wx     = {};
    for (int i = 0; i < 3; i++) {
        s_lat[i]      = 0.0f;
        s_lon[i]      = 0.0f;
        s_geocoded[i] = false;
        cfg_locations[i][0] = '\0';
    }
    cfg_nLocations = 0;
    s_locIdx       = 0;
    memset(cfg_ssid, 0, sizeof(cfg_ssid));
    memset(cfg_pass, 0, sizeof(cfg_pass));
    bootWas        = false;
    bootDownAt     = 0;
    bootLongFired  = false;
    lastPwrPoll    = 0;
    lastFetch      = 0;
    lastDraw       = 0;

    showStatus("Weather Dashboard", "Starting...");
    delay(400);

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (!SD_MMC.begin("/sdcard", true)) {
        showStatus("SD card failed!", "Check card is inserted");
        delay(5000);
        return;
    }
    if (!readConfig()) {
        showStatus("Config missing!", "/setup/setup.txt");
        delay(8000);
        return;
    }

    if (!wifiConnect()) {
        showStatus("WiFi failed!", "Check credentials");
        delay(8000);
        return;
    }
    USBSerial.println("WiFi connected");

    showStatus("Locating city...", cfg_locations[s_locIdx]);
    if (!geocodeCity(s_locIdx)) {
        showStatus("City not found!", cfg_locations[s_locIdx]);
        delay(5000);
        return;
    }

    showStatus("Fetching weather...", "api.open-meteo.com");
    if (fetchWeather()) drawDashboard();
    else delay(8000);   // error already shown by fetchWeather()
    lastFetch = millis();
}

void app20_loop() {
    common_tick();
    uint32_t now = millis();

    // BOOT: short = rotate portrait/landscape, long (≥800 ms) = manual fetch
    bool bootDown = (digitalRead(BOOT_BTN) == LOW);
    if (bootDown && !bootWas) {
        bootDownAt    = now;
        bootLongFired = false;
    }
    if (bootDown && !bootLongFired && (now - bootDownAt) >= BOOT_LONG_MS) {
        bootLongFired = true;
        common_activity();
        showStatus("Refreshing...", cfg_locations[s_locIdx]);
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            delay(2000);
        }
        if (WiFi.status() == WL_CONNECTED && fetchWeather()) {
            drawDashboard();
            lastFetch = now;
        }
    }
    if (!bootDown && bootWas && !bootLongFired) {
        common_activity();
        s_rot ^= 1;
        if (wx.valid) drawDashboard();
        else showStatus("Weather Dashboard", "Loading...");
    }
    bootWas = bootDown;

    // PWR short press: cycle through configured cities
    if (common_consume_pwr_short() && cfg_nLocations > 1) {
        common_activity();
        s_locIdx = (s_locIdx + 1) % cfg_nLocations;
        if (!s_geocoded[s_locIdx]) {
            showStatus("Locating city...", cfg_locations[s_locIdx]);
            if (!geocodeCity(s_locIdx)) {
                showStatus("City not found!", cfg_locations[s_locIdx]);
                delay(1500);
            }
        }
        showStatus("Switching city...", cfg_locations[s_locIdx]);
        lastFetch = 0;   // triggers re-fetch immediately
    }

    // Periodic data refresh
    if (now - lastFetch >= s_fetchMs) {
        lastFetch = now;
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            delay(3000);
        }
        if (WiFi.status() == WL_CONNECTED && fetchWeather())
            drawDashboard();
    }

    // Redraw every minute to keep clock current
    if (wx.valid && now - lastDraw >= DRAW_MS)
        drawDashboard();

    delay(100);
}
