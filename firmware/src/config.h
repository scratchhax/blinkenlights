// config.h - panel wiring and limits. Edit to match your hardware.
// Wi-Fi credentials and the OTA password live in secrets.h (copy secrets.example.h).
#pragma once

// GPIO driving the panel's DIN, through a 74AHCT125 level shifter if you have one.
// platformio.ini can override it per board with BOARD_DATA_PIN (the XIAO uses GPIO 1, marked D0).
// (Not DATA_PIN itself: FastLED uses that name internally, so it can't be a global build flag.)
#ifdef BOARD_DATA_PIN
#define DATA_PIN BOARD_DATA_PIN
#else
#define DATA_PIN 13
#endif

// Overall brightness ceiling (0..255) and the power budget FastLED enforces.
// 256 LEDs at full white draw about 15 A; 2500 mA keeps a 5 V 3 A supply happy and still looks bright.
#define BRIGHTNESS 255
#define MAX_MILLIAMPS 2500
// How much to darken the dim end. 2.2 is "correct" but WS2812s lose the faint layers (twinkle,
// trails) below their first few steps; 1.6 keeps them visible.
#define GAMMA 1.6f

// How the 256 LEDs are chained. Most 16x16 panels snake: row 0 left to right, row 1 right to left, ...
#define SERPENTINE 1
// Flip the picture if the panel is mounted rotated or mirrored.
#define FLIP_X 0
#define FLIP_Y 0
// Some panels snake by column instead of row.
#define COLUMN_MAJOR 0

// Network name: the panel answers as blinkpanel.local, for blinkd and for over-the-air flashing.
#define HOSTNAME "blinkpanel"
// Wi-Fi transmit power. Full power is best for range; drop to WIFI_POWER_11dBm only if a weak
// 5 V supply makes the board reset when Wi-Fi starts.
#define WIFI_TX_POWER WIFI_POWER_19_5dBm
// Test switch: 1 stops the chip resetting itself when its 3.3 V rail dips (the "brownout" reset).
// If the board only works with this on, the supply or wiring sags under Wi-Fi load; fix that and set it back to 0.
#define DISABLE_BROWNOUT 0
// Photosensitive safety. The panel always stops any sizeable patch from flashing more than about
// twice a second (the WCAG / Harding rule). Strict mode also limits every single LED to under 3
// flashes a second; it is safer still but makes the animation look stuttery.
#define PHOTOSAFE_STRICT 0
#define UDP_PORT 4210

// USB serial still works alongside Wi-Fi, handy for the first flash or when the network is down.
#define SERIAL_BAUD 921600
