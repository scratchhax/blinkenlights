// main.cpp - ESP32 side: receive signal frames from blinkd over Wi-Fi (UDP) or USB serial,
// animate the 16x16 panel at 60 fps. The animation itself lives in lib/blink (shared with the
// desktop preview). After the first USB flash, new firmware goes over Wi-Fi (ArduinoOTA).
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>

#include "blink_protocol.h"
#include "blink_renderer.h"
#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy src/secrets.example.h to src/secrets.h and fill in your Wi-Fi details"
#endif

static CRGB leds[256];
static blink::Renderer renderer;
static blink::FrameParser serialParser, udpParser;
static blink::RGB pixels[blink::W * blink::H];
static float gammaF[256];    // perceptual 0..255 -> linear 0..1
static uint8_t gammaLut[256];  // the same with overall brightness applied, rebuilt when it changes
static uint8_t brightNow = 0;
static WiFiUDP udp;
static bool udpStarted = false;
static uint32_t udpFrames = 0, serialFrames = 0;
static uint32_t lastFrameAt = 0, maxFrameGap = 0, maxStall = 0;  // for the status line
static uint32_t lastGap10s = 0, lastStall10s = 0, worstGap = 0, worstStall = 0;
static esp_reset_reason_t resetWhy;
static WebServer web(80);

static void gotFrame(uint32_t now) {
  if (lastFrameAt && now - lastFrameAt > maxFrameGap) maxFrameGap = now - lastFrameAt;
  lastFrameAt = now;
}

// Logical (x, y), (0,0) top left, to the LED's position in the chain.
static uint16_t ledIndex(int x, int y) {
  if (FLIP_X) x = 15 - x;
  if (FLIP_Y) y = 15 - y;
  if (COLUMN_MAJOR) {
    int t = x; x = y; y = t;
  }
  if (SERPENTINE && (y & 1)) x = 15 - x;
  return (uint16_t)(y * 16 + x);
}

// While new firmware streams in, the outer ring fills up as a progress bar.
static void showOtaProgress(unsigned int done, unsigned int total) {
  static const uint8_t ring[60][2] = {
      {0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}, {8, 0}, {9, 0}, {10, 0}, {11, 0},
      {12, 0}, {13, 0}, {14, 0}, {15, 0}, {15, 1}, {15, 2}, {15, 3}, {15, 4}, {15, 5}, {15, 6}, {15, 7},
      {15, 8}, {15, 9}, {15, 10}, {15, 11}, {15, 12}, {15, 13}, {15, 14}, {15, 15}, {14, 15}, {13, 15},
      {12, 15}, {11, 15}, {10, 15}, {9, 15}, {8, 15}, {7, 15}, {6, 15}, {5, 15}, {4, 15}, {3, 15},
      {2, 15}, {1, 15}, {0, 15}, {0, 14}, {0, 13}, {0, 12}, {0, 11}, {0, 10}, {0, 9}, {0, 8}, {0, 7},
      {0, 6}, {0, 5}, {0, 4}, {0, 3}, {0, 2}, {0, 1}};
  int lit = total ? (int)((uint64_t)done * 60 / total) : 0;
  FastLED.clear();
  for (int i = 0; i < lit; i++) leds[ledIndex(ring[i][0], ring[i][1])] = CRGB(0, 40, 80);
  FastLED.show();
}

static void startOta() {
  ArduinoOTA.setHostname(HOSTNAME);  // also announces blinkpanel.local over mDNS
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { FastLED.clear(true); });
  ArduinoOTA.onProgress(showOtaProgress);
  ArduinoOTA.begin();
}

// Survives resets (not power cycles): how many times the board has restarted since power-up.
RTC_NOINIT_ATTR static uint32_t bootCount;

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply voltage sagged)";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_EXT: return "EN button / serial port";
    default: return "other";
  }
}

void setup() {
#if DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
  Serial.setRxBufferSize(2048);
  Serial.begin(SERIAL_BAUD);
  esp_reset_reason_t why = resetWhy = esp_reset_reason();
  if (why == ESP_RST_POWERON) bootCount = 0;
  bootCount++;
  Serial.printf("blinkpanel: boot %u, reset reason: %s\n", (unsigned)bootCount, resetName(why));

  // The renderer works in perceptual brightness; LEDs are linear, so apply gamma here.
  for (int i = 0; i < 256; i++) gammaF[i] = powf(i / 255.0f, GAMMA);

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, 256);
  FastLED.setBrightness(255);  // overall brightness lives in gammaLut instead (see loop)
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.clear(true);

  renderer.begin(esp_random());
  renderer.setPhotosafe(true, PHOTOSAFE_STRICT);

  // Connect in the background: the standby shimmer runs until frames arrive.
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("blinkpanel: Wi-Fi lost, reason %d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    Serial.printf("blinkpanel: Wi-Fi up at %s\n", WiFi.localIP().toString().c_str());
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  // Core 2.0.x only sends our hostname in DHCP reliably with this config() call first;
  // otherwise the router sometimes sees the default esp32-xxxxxx name.
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(HOSTNAME);  // before mode(): some core versions ignore it afterwards
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep adds 100+ ms of jitter to incoming frames
  WiFi.setTxPower(WIFI_TX_POWER);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Wi-Fi watchdog. Auto-reconnect sometimes gives up after a failed attempt and never tries again,
// so if the link stays down we start over, and after two minutes restart the whole chip.
static void keepWifiUp(uint32_t now) {
  static uint32_t downSince = 0, lastKick = 0;
  if (WiFi.status() == WL_CONNECTED) {
    downSince = 0;
    return;
  }
  if (!downSince) downSince = lastKick = now;
  if (now - downSince > 120000) {
    Serial.println("blinkpanel: no Wi-Fi for 2 minutes, restarting");
    delay(50);
    ESP.restart();
  }
  if (now - lastKick > 15000) {
    Serial.println("blinkpanel: Wi-Fi still down, reconnecting");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastKick = now;
  }
}

// A plain-text status page at http://<panel address>/ for when no USB cable is attached.
static void startWeb() {
  web.on("/", []() {
    char buf[700];
    uint32_t up = millis() / 1000;
    snprintf(buf, sizeof buf,
             "blinkpanel\n"
             "address        %s\n"
             "Wi-Fi signal   %d dBm\n"
             "up for         %uh %02um %02us (boot %u, last reset: %s)\n"
             "frames         %u over Wi-Fi, %u over USB\n"
             "last frame     %u ms ago\n"
             "frame gap      longest %u ms in the last 10 s, %u ms ever (20 a second = 50 ms)\n"
             "render stall   longest %u ms in the last 10 s, %u ms ever (60 fps = 16 ms)\n"
             "brightness     %u of 255%s\n"
             "free memory    %u bytes\n",
             WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (unsigned)(up / 3600),
             (unsigned)(up / 60 % 60), (unsigned)(up % 60), (unsigned)bootCount, resetName(resetWhy),
             (unsigned)udpFrames, (unsigned)serialFrames, (unsigned)(lastFrameAt ? millis() - lastFrameAt : 0),
             (unsigned)lastGap10s, (unsigned)worstGap, (unsigned)lastStall10s, (unsigned)worstStall,
             (unsigned)brightNow, renderer.hostBrightness() ? " (set by the host)" : " (config.h)",
             (unsigned)ESP.getFreeHeap());
    web.send(200, "text/plain", buf);
  });
  web.begin();
}

static void pollNetwork() {
  keepWifiUp(millis());
  if (WiFi.status() != WL_CONNECTED) return;
  if (!udpStarted) {  // first connection: open the frame port and the OTA listener
    udp.begin(UDP_PORT);
    startOta();
    startWeb();
    udpStarted = true;
    Serial.printf("blinkpanel: %s on %s:%d (boot %u)\n", HOSTNAME, WiFi.localIP().toString().c_str(), UDP_PORT,
                  (unsigned)bootCount);
  }
  ArduinoOTA.handle();
  web.handleClient();
  uint32_t now = millis();
  while (udp.parsePacket() > 0) {
    uint8_t buf[256];
    int len = udp.read(buf, sizeof buf);
    for (int i = 0; i < len; i++)
      if (udpParser.feed(buf[i])) {
        renderer.onFrame(udpParser.frame(), now);
        udpFrames++;
        gotFrame(now);
      }
  }
}

static void pollSerial() {
  uint32_t now = millis();
  while (Serial.available() > 0)
    if (serialParser.feed((uint8_t)Serial.read())) {
      renderer.onFrame(serialParser.frame(), now);
      serialFrames++;
      gotFrame(now);
    }
}

void loop() {
  static uint32_t nextFrame = 0;
  pollNetwork();
  pollSerial();

  uint32_t now = millis();
  // A status line every 10 s, so a serial monitor opened at any time shows what's going on.
  static uint32_t nextStatus = 10000;
  if ((int32_t)(now - nextStatus) >= 0) {
    nextStatus = now + 10000;
    if (WiFi.status() == WL_CONNECTED)
      Serial.printf("blinkpanel: on Wi-Fi at %s, signal %d dBm, frames received: %u Wi-Fi, %u USB, "
                    "longest frame gap %u ms, longest stall %u ms\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (unsigned)udpFrames, (unsigned)serialFrames,
                    (unsigned)maxFrameGap, (unsigned)maxStall);
    else
      Serial.printf("blinkpanel: not on Wi-Fi (status %d)\n", (int)WiFi.status());
    lastGap10s = maxFrameGap;
    lastStall10s = maxStall;
    if (maxFrameGap > worstGap) worstGap = maxFrameGap;
    if (maxStall > worstStall) worstStall = maxStall;
    maxFrameGap = maxStall = 0;
  }
  if ((int32_t)(now - nextFrame) < 0) return;
  static uint32_t lastRender = 0;
  if (lastRender && now - lastRender > maxStall) maxStall = now - lastRender;  // >17 ms = the board stalled
  lastRender = now;
  nextFrame = now + 16;  // ~60 fps; one show() of 256 LEDs takes about 8 ms

  // Overall brightness: the host's setting when it sends one, else BRIGHTNESS from config.h.
  // Eased over about half a second so a change never looks like a flash.
  static float bright = BRIGHTNESS;
  uint8_t want = renderer.hostBrightness();
  bright += ((want ? want : BRIGHTNESS) - bright) * 0.05f;
  // Brightness is applied before rounding to the LED's 0..255 steps, not after like
  // FastLED.setBrightness, so a dim panel still shows faint things: anything the renderer meant
  // to be visible keeps at least the LED's lowest step instead of rounding to black.
  uint8_t b = (uint8_t)(bright + 0.5f);
  if (b != brightNow) {
    brightNow = b;
    int floorStep = (3 * b + 127) / 255;
    if (floorStep < 1) floorStep = 1;
    for (int i = 0; i < 256; i++) {
      int v = (int)(gammaF[i] * b + 0.5f);
      if (i >= 12 && v < floorStep) v = floorStep;
      gammaLut[i] = (uint8_t)v;
    }
  }

  renderer.render(now, pixels);
  for (int y = 0; y < blink::H; y++)
    for (int x = 0; x < blink::W; x++) {
      const blink::RGB& p = pixels[y * blink::W + x];
      leds[ledIndex(x, y)] = CRGB(gammaLut[p.r], gammaLut[p.g], gammaLut[p.b]);
    }
  // Not on Wi-Fi: the top-left LED blinks red, so a board with no screen still says what's wrong.
  if (WiFi.status() != WL_CONNECTED) leds[ledIndex(0, 0)] = (now / 500) % 2 ? CRGB(60, 0, 0) : CRGB(0, 0, 0);
  FastLED.show();
}
