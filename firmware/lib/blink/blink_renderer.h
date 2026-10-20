// blink_renderer.h - turns signal frames into 60 fps blinkenlights on a 16x16 grid.
//
// Pure C++ with no Arduino or FastLED dependency, so the same code runs on the ESP32
// and in the desktop preview. Output is a logical 16x16 grid, row-major, (0,0) top left,
// in perceptual brightness; the firmware applies gamma and the panel's wiring order.
//
// Layout (see the design doc):
//   ring   - the 60 outer LEDs: request pulses, orbiting request and connection dots, context and RAM fill
//   glows  - seven soft glows bleeding in from the inside edge of the ring, one per subsystem
//            (clockwise from top left): GPU, VRAM bandwidth, GPU power, CPU, disk, network, temp
//   field  - everything inside the ring: tokens, synapses, prompt rain, disk and memory streaks
#pragma once
#include <stdint.h>
#include "blink_protocol.h"

namespace blink {

constexpr int W = 16;
constexpr int H = 16;
constexpr int RING_LEN = 60;

struct RGB { uint8_t r, g, b; };
struct FRGB { float r, g, b; };

class Renderer {
 public:
  void begin(uint32_t seed);
  void onFrame(const Frame& f, uint32_t nowMs);
  // Advance the animation to nowMs and write 256 pixels, row-major.
  void render(uint32_t nowMs, RGB out[W * H]);
  bool standby() const { return standbyMix_ > 0.5f; }
  // Brightness the host asked for in the last frame, 1..255, or 0 for the panel's own setting.
  uint8_t hostBrightness() const { return haveFrame_ ? cur_.brightness : 0; }
  // Photosensitive safety limiter, on by default; see limitFlashes().
  void setPhotosafe(bool on, bool strict = false) { photosafe_ = on; strictFlash_ = strict; }

 private:
  struct Drop { float x, y, v; FRGB c; bool on; bool soft; };  // soft: glides, no hard head
  struct Ripple { float r; bool on; };
  struct Twinkle { int x, y; FRGB c; float k, age; bool on; };
  struct Comet { float pos; float left; bool on; };
  // A signal racing from one neuron to another; on arrival it flashes and may fire onward.
  struct Synapse { float x, y, tx, ty, vx, vy; FRGB c; int hops; bool on; };

  float rnd();
  int irnd(int n);
  void add(FRGB (&buf)[H][W], int x, int y, FRGB c, float k);
  void lighten(int x, int y, FRGB c, float k);
  void ringXY(int i, int& x, int& y) const;
  FRGB primary(float jitter);
  FRGB accent();

  void updateInputs(float dt);
  void drawField(float dt, float t);
  void fireSynapse(float x, float y, FRGB c, int hops);
  void twinkle(int x, int y, FRGB c, float k);
  void drawTwinkles(float dt);
  void drawRing(float dt);
  void drawLogic(float dt);
  void drawStandby(float t);
  void limitFlashes(float dt, float t, FRGB (&pix)[H][W]);

  uint32_t rng_ = 1;
  uint32_t startMs_ = 0, lastMs_ = 0, lastFrameMs_ = 0;
  bool haveFrame_ = false, havePrev_ = false;
  Frame cur_{};
  uint16_t prev_[C_COUNT] = {};

  // smoothed inputs, 0..1
  float lv_[L_COUNT] = {};
  float cores_[N_CORES] = {};
  float gpus_[N_GPUS] = {};
  float threads_[N_THREADS] = {};

  // events waiting to be spent over the next frames
  float pendTokens_ = 0, pendPrompt_ = 0, pendDrafts_ = 0, pendDisk_ = 0;
  uint32_t lastTokMs_ = 0;
  float tokGap_ = 0.05f;  // learned seconds between token updates from the host
  int pendStarts_ = 0, pendEnds_ = 0;

  FRGB acc_[H][W] = {};   // persistent layer with trails
  FRGB base_[H][W] = {};  // redrawn every frame

  Drop drops_[40] = {};
  Ripple ripples_[4] = {};
  Twinkle twinkles_[32] = {};
  Comet comets_[4] = {};
  float orbit_ = 0, queueOrbit_ = 0;
  Synapse synapses_[10] = {};
  float synapseCredit_ = 0;
  float breath_ = 0, connOrbit_ = 0;
  float standbyMix_ = 1.0f;
  float waveMix_ = 1;  // the rainbow wave: only while no frames arrive (the collector is gone)
  bool photosafe_ = true;
  bool strictFlash_ = false;
  float patchLum_[4][4] = {};
  float pixLow_[H][W] = {}, pixJump_[H][W] = {};
  float rippleGap_ = 0;
  float glowLv_[7] = {}, glowShare_[2] = {};
};

}  // namespace blink
