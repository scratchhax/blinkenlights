// blink_renderer.cpp - see blink_renderer.h for the layout.
#include "blink_renderer.h"

#include <math.h>
#include <string.h>

namespace blink {

namespace {

constexpr int FX0 = 1, FX1 = 14, FY0 = 1, FY1 = 14;  // neural field bounds (inclusive)
constexpr float FIELD_TAU = 0.20f;                   // seconds for a spark to fade to 37 %
constexpr float RING_TAU = 0.15f;
constexpr uint32_t STANDBY_AFTER_MS = 2000;          // no frames this long: standby shimmer

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float minf(float a, float b) { return a < b ? a : b; }
inline float maxf(float a, float b) { return a > b ? a : b; }

FRGB hsv(float h, float s, float v) {
  h = h - floorf(h);
  float i = floorf(h * 6), f = h * 6 - i;
  float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  switch ((int)i % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
  }
}

inline FRGB scale(FRGB c, float k) { return {c.r * k, c.g * k, c.b * k}; }

bool isRing(int x, int y) { return x == 0 || y == 0 || x == W - 1 || y == H - 1; }

// How many of `pending` events to spend this frame: about 40 ms worth, at most `cap`.
int spend(float& pending, float dt, int cap, float roll) {
  float want = pending * minf(1.0f, dt * 25.0f);
  int n = (int)want;
  if (roll < want - n) n++;
  if (n > cap) n = cap;
  if (n > pending) n = (int)pending;
  pending -= n;
  if (pending < 0) pending = 0;
  return n;
}

}  // namespace

void Renderer::begin(uint32_t seed) {
  rng_ = seed ? seed : 0x9E3779B9u;
}

float Renderer::rnd() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return (rng_ >> 8) * (1.0f / 16777216.0f);
}

int Renderer::irnd(int n) { int v = (int)(rnd() * n); return v >= n ? n - 1 : v; }

void Renderer::add(FRGB (&buf)[H][W], int x, int y, FRGB c, float k) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  buf[y][x].r += c.r * k;
  buf[y][x].g += c.g * k;
  buf[y][x].b += c.b * k;
}

// Ring index 0..59 runs clockwise from the top-left corner.
// Brighten a cell toward c without adding past it, so overlapping drops never wash out to white.
void Renderer::lighten(int x, int y, FRGB c, float k) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  FRGB& a = acc_[y][x];
  a = {maxf(a.r, c.r * k), maxf(a.g, c.g * k), maxf(a.b, c.b * k)};
}

void Renderer::ringXY(int i, int& x, int& y) const {
  i %= RING_LEN;
  if (i < 0) i += RING_LEN;
  if (i < 16) { x = i; y = 0; return; }
  i -= 16;
  if (i < 15) { x = 15; y = 1 + i; return; }
  i -= 15;
  if (i < 15) { x = 14 - i; y = 15; return; }
  i -= 15;
  x = 0; y = 14 - i;
}

// llama.cpp leans blue and white, strata amber and gold, Maya teal-green, nothing violet.
FRGB Renderer::primary(float jitter) {
  float j = (rnd() - 0.5f) * jitter;
  switch (cur_.engine) {
    case ENGINE_LLAMA: return hsv(0.60f + j, 0.75f, 1);
    case ENGINE_STRATA: return hsv(0.09f + j, 0.92f, 1);
    case ENGINE_MAYA: return hsv(0.46f + j, 0.85f, 1);
    default: return hsv(0.78f + j, 0.7f, 1);
  }
}

FRGB Renderer::accent() {
  switch (cur_.engine) {
    case ENGINE_LLAMA: return hsv(0.55f, 0.15f, 1);
    case ENGINE_STRATA: return hsv(0.13f, 0.45f, 1);
    case ENGINE_MAYA: return hsv(0.43f, 0.22f, 1);
    default: return hsv(0.85f, 0.3f, 1);
  }
}

void Renderer::onFrame(const Frame& f, uint32_t nowMs) {
  bool fresh = haveFrame_ && nowMs - lastFrameMs_ < 3000;
  if (havePrev_ && fresh) {
    uint16_t d[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) d[i] = (uint16_t)(f.counters[i] - prev_[i]);
    pendTokens_ = minf(pendTokens_ + d[C_TOKENS], 400);
    // Engines often report tokens in bursts (every second or two). Learn how far apart the bursts
    // are, so they can be played out evenly over that gap instead of all at once.
    if (d[C_TOKENS] > 0) {
      float gap = (nowMs - lastTokMs_) / 1000.0f;
      if (lastTokMs_ && gap < 4.0f) tokGap_ += (gap - tokGap_) * 0.5f;
      lastTokMs_ = nowMs;
    }
    pendPrompt_ = minf(pendPrompt_ + d[C_PROMPT_TOKENS], 20);
    pendDrafts_ = minf(pendDrafts_ + d[C_DRAFTS_ACCEPTED], 400);
    pendDisk_ = minf(pendDisk_ + d[C_DISK_BURSTS], 30);
    pendStarts_ += d[C_REQ_START] > 4 ? 4 : d[C_REQ_START];
    pendEnds_ += d[C_REQ_END] > 4 ? 4 : d[C_REQ_END];
    if (pendStarts_ > 4) pendStarts_ = 4;
    if (pendEnds_ > 4) pendEnds_ = 4;
  }
  memcpy(prev_, f.counters, sizeof prev_);
  havePrev_ = true;
  cur_ = f;
  haveFrame_ = true;
  lastFrameMs_ = nowMs;
}

void Renderer::updateInputs(float dt) {
  float k = 1.0f - expf(-dt / 0.15f);
  for (int i = 0; i < L_COUNT; i++) lv_[i] += (cur_.levels[i] / 255.0f - lv_[i]) * k;
  for (int i = 0; i < N_CORES; i++) cores_[i] += (cur_.cores[i] / 255.0f - cores_[i]) * k;
  for (int i = 0; i < N_GPUS; i++) gpus_[i] += (cur_.gpus[i] / 255.0f - gpus_[i]) * k;
  for (int i = 0; i < N_THREADS; i++) threads_[i] += (cur_.threads[i] / 255.0f - threads_[i]) * k;
}

void Renderer::drawField(float dt, float t) {
  (void)t;
  const float act = 1.0f - standbyMix_;
  if (act <= 0.01f) return;

  // Background twinkle: each column follows one engine thread (and a CPU core). The whole field
  // breathes, faster with load average and brighter with CPU clock speed.
  breath_ += dt * 6.2831853f * (0.12f + 0.9f * lv_[L_LOADAVG]);
  if (breath_ > 6283.1853f) breath_ -= 6283.1853f;
  float breath = (0.55f + 0.45f * sinf(breath_)) * (0.7f + 0.5f * lv_[L_CPU_FREQ]);
  const bool idle = standbyMix_ > 0.3f;  // idle sparks fade up and down instead of flashing on
  for (int c = 0; c < N_THREADS; c++) {
    float rate = (18.0f * threads_[c] + 2.5f * cores_[c % N_CORES]) * (0.7f + 0.8f * lv_[L_LOADAVG]);
    if (rnd() < rate * dt) {
      int y = FY0 + irnd(FY1 - FY0 + 1);
      float k = (0.18f + 0.4f * threads_[c]) * breath * act;
      if (idle) twinkle(FX0 + c, y, primary(0.08f), k);
      else add(acc_, FX0 + c, y, primary(0.08f), k);
    }
  }

  // One spark per generated token. strata and Maya: blue for an expert cache hit, amber for a miss.
  bool hitKnown = (cur_.engine == ENGINE_STRATA || cur_.engine == ENGINE_MAYA) && cur_.levels[L_EXPERT_HIT] != 255;
  // Play pending tokens out over the usual gap between bursts (at least 40 ms worth per frame).
  float drain = minf(1.0f, dt / maxf(0.04f, minf(tokGap_, 2.5f)));
  int n = 0;
  {
    float want = pendTokens_ * drain;
    n = (int)want;
    if (rnd() < want - n) n++;
    if (n > 16) n = 16;
    if (n > pendTokens_) n = (int)pendTokens_;
    pendTokens_ -= n;
  }
  synapseCredit_ = minf(synapseCredit_ + n * 0.12f, 4.0f);
  for (int i = 0; i < n; i++) {
    int x = FX0 + irnd(FX1 - FX0 + 1), y = FY0 + irnd(FY1 - FY0 + 1);
    FRGB c;
    if (hitKnown) c = rnd() < lv_[L_EXPERT_HIT] ? hsv(0.56f, 0.7f, 1) : hsv(0.07f, 0.95f, 1);
    else c = rnd() < 0.7f ? primary(0.06f) : accent();
    add(acc_, x, y, c, act);
    // An accepted speculative draft makes the spark jump to neighbors, like a synapse.
    if (pendDrafts_ >= 1) {
      pendDrafts_ -= 1;
      for (int j = 0; j < 2; j++) {
        int nx = x + irnd(3) - 1, ny = y + irnd(3) - 1;
        if (nx >= FX0 && nx <= FX1 && ny >= FY0 && ny <= FY1) add(acc_, nx, ny, c, 0.55f * act);
      }
    }
  }
  if (n == 0 && pendTokens_ < 1) pendDrafts_ = minf(pendDrafts_, 8);

  // Prompt rain (cyan, fast) and disk streaks (amber, slow).
  int rain = rnd() < 0.5f ? spend(pendPrompt_, dt, 1, rnd()) : 0;  // at most ~30 drops a second
  float diskRate = 10.0f * lv_[L_DISK_READ] * lv_[L_DISK_READ] + 4.0f * lv_[L_MAJ_FAULTS];
  int disk = spend(pendDisk_, dt, 2, rnd()) + (rnd() < diskRate * dt ? 1 : 0);
  // Memory: violet streaks for page-cache churn, hot pink for swap.
  int paging = rnd() < 8.0f * lv_[L_PAGING] * lv_[L_PAGING] * dt ? 1 : 0;
  int swap = rnd() < (0.5f + 12.0f * lv_[L_SWAP]) * (lv_[L_SWAP] > 0.02f) * dt ? 1 : 0;
  for (Drop& d : drops_) {
    if (d.on) continue;
    if (rain > 0) {
      d = {(float)(FX0 + irnd(FX1 - FX0 + 1)), FY0 - 1.5f, 14.0f + rnd() * 10.0f, hsv(0.5f, 0.6f, 0.9f), true, true};
      rain--;
    } else if (disk > 0) {
      d = {(float)(FX0 + irnd(FX1 - FX0 + 1)), FY0 - 1.5f, 5.0f + rnd() * 4.0f, hsv(0.07f, 1.0f, 0.75f), true, true};
      disk--;
    } else if (swap > 0) {
      d = {(float)(FX0 + irnd(FX1 - FX0 + 1)), FY0 - 1.5f, 9.0f + rnd() * 5.0f, hsv(0.92f, 0.8f, 0.9f), true, true};
      swap--;
    } else if (paging > 0) {
      d = {(float)(FX0 + irnd(FX1 - FX0 + 1)), FY0 - 1.5f, 7.0f + rnd() * 5.0f, hsv(0.76f, 0.75f, 0.6f), true, true};
      paging--;
    } else {
      break;
    }
  }
  for (Drop& d : drops_) {
    if (!d.on) continue;
    float y0 = d.y;
    d.y += d.v * dt;
    if (d.soft) {
      // Streaks glide: the head is shared between cells, the next cells down fade up
      // before it arrives, and a short tail fades out behind. They start above the field and
      // run out below it, so they slide in and out rather than appearing.
      constexpr float AHEAD = 1.5f, TAIL = 4.0f;
      for (int y = (int)floorf(d.y - TAIL); y <= (int)ceilf(d.y + AHEAD); y++) {
        if (y < FY0 || y > FY1) continue;
        float dist = d.y - y;  // positive: behind the head
        float k = dist < 0 ? (1.0f + dist / AHEAD) * (1.0f + dist / AHEAD) : (1.0f - dist / TAIL) * (1.0f - dist / TAIL);
        if (k > 0) add(base_, (int)d.x, y, d.c, 0.8f * k * act);
      }
      if (d.y > FY1 + TAIL + 1) d.on = false;
      continue;
    }
    for (int y = (int)y0 + 1; y <= (int)d.y && y <= FY1; y++) lighten((int)d.x, y, d.c, act);
    if (d.y > FY1 + 1) d.on = false;
  }

  // Synapses: every few tokens a signal races between two neurons, flashes, and sometimes
  // fires onward. Faster generation means more of them in flight.
  while (synapseCredit_ >= 1.0f) {
    synapseCredit_ -= 1.0f;
    fireSynapse((float)(FX0 + irnd(FX1 - FX0 + 1)), (float)(FY0 + irnd(FY1 - FY0 + 1)),
                rnd() < 0.75f ? primary(0.05f) : accent(), 0);
  }
  for (Synapse& sy : synapses_) {
    if (!sy.on) continue;
    float remain = fabsf(sy.tx - sy.x) + fabsf(sy.ty - sy.y);
    float step = sqrtf(sy.vx * sy.vx + sy.vy * sy.vy) * dt;
    int steps = 1 + (int)(step * 2);
    for (int k = 0; k < steps && sy.on; k++) {
      sy.x += sy.vx * dt / steps;
      sy.y += sy.vy * dt / steps;
      lighten((int)(sy.x + 0.5f), (int)(sy.y + 0.5f), sy.c, 0.75f * act);
      float now = fabsf(sy.tx - sy.x) + fabsf(sy.ty - sy.y);
      if (now > remain || now < 0.3f) {  // arrived (or overshot): the neuron fires
        int x = (int)(sy.tx + 0.5f), y = (int)(sy.ty + 0.5f);
        add(acc_, x, y, {1, 1, 1}, 0.8f * act);
        add(acc_, x - 1, y, sy.c, 0.45f * act);
        add(acc_, x + 1, y, sy.c, 0.45f * act);
        add(acc_, x, y - 1, sy.c, 0.45f * act);
        add(acc_, x, y + 1, sy.c, 0.45f * act);
        sy.on = false;
        if (sy.hops < 3 && rnd() < 0.45f) fireSynapse(sy.tx, sy.ty, sy.c, sy.hops + 1);
      }
      remain = now;
    }
  }

  // PCIe shimmer: a faint line across a random row.
  if (rnd() < lv_[L_PCIE] * 6.0f * dt) {
    int y = FY0 + irnd(FY1 - FY0 + 1);
    FRGB c = accent();
    for (int x = FX0; x <= FX1; x++) {
      if (idle) twinkle(x, y, c, 0.12f * act);
      else add(acc_, x, y, c, 0.12f * act);
    }
  }

  // A finished request ripples out from the center across the whole panel.
  // (No more than one every 0.6 s: a ripple sweeps a big area, so a burst of them would strobe.)
  rippleGap_ -= dt;
  for (Ripple& r : ripples_) {
    if (!r.on && pendEnds_ > 0 && rippleGap_ <= 0) { r = {0, true}; pendEnds_--; rippleGap_ = 0.6f; }
  }
  for (Ripple& r : ripples_) {
    if (!r.on) continue;
    r.r += 11.0f * dt;
    float k = 0.45f * (1.0f - r.r / 12.0f) * act;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        float dx = x + 0.5f - 8.0f, dy = y + 0.5f - 8.0f;
        if (fabsf(sqrtf(dx * dx + dy * dy) - r.r) < 0.6f) add(base_, x, y, {1, 1, 1}, k);
      }
    if (r.r > 12.0f) r.on = false;
  }
}

// A soft spark: fades up over a third of a second and back down over most of a second, so the
// quiet moments sparkle without anything flashing on. If every slot is busy the spark is dropped.
void Renderer::twinkle(int x, int y, FRGB c, float k) {
  for (Twinkle& tw : twinkles_) {
    if (tw.on) continue;
    tw = {x, y, c, k, 0, true};
    return;
  }
}

void Renderer::drawTwinkles(float dt) {
  constexpr float UP = 0.35f, DOWN = 0.8f;
  for (Twinkle& tw : twinkles_) {
    if (!tw.on) continue;
    tw.age += dt;
    float e;
    if (tw.age < UP) {
      float u = tw.age / UP;
      e = u * u * (3 - 2 * u);
    } else {
      float u = (tw.age - UP) / DOWN;
      if (u >= 1) { tw.on = false; continue; }
      e = (1 - u) * (1 - u);
    }
    add(base_, tw.x, tw.y, tw.c, tw.k * e);
  }
}

void Renderer::fireSynapse(float x, float y, FRGB c, int hops) {
  for (Synapse& sy : synapses_) {
    if (sy.on) continue;
    float tx = x, ty = y;
    for (int tries = 0; tries < 6; tries++) {  // a target 3..8 cells away, inside the field
      float ang = rnd() * 6.2831853f, d = 3.0f + rnd() * 5.0f;
      tx = x + cosf(ang) * d;
      ty = y + sinf(ang) * d;
      if (tx >= FX0 && tx <= FX1 && ty >= FY0 && ty <= FY1) break;
    }
    tx = tx < FX0 ? FX0 : (tx > FX1 ? FX1 : tx);
    ty = ty < FY0 ? FY0 : (ty > FY1 ? FY1 : ty);
    float dx = tx - x, dy = ty - y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1.5f) return;
    float speed = 28.0f + rnd() * 22.0f;
    sy = {x, y, roundf(tx), roundf(ty), dx / len * speed, dy / len * speed, c, hops, true};
    return;
  }
}

void Renderer::drawRing(float dt) {
  const float act = 1.0f - standbyMix_;
  if (act <= 0.01f) return;
  int x, y;

  // Context fill: a dim green arc, clockwise from the top-left corner.
  float fill = lv_[L_CONTEXT] * RING_LEN;
  FRGB green = hsv(0.33f, 0.9f, 1);
  for (int i = 0; i < RING_LEN && i < fill + 1; i++) {
    ringXY(i, x, y);
    add(base_, x, y, green, 0.10f * minf(1.0f, fill - i) * act);
  }
  // RAM used: a dim violet arc the other way round, counter-clockwise from the same corner.
  float ram = lv_[L_MEM] * RING_LEN;
  FRGB violet = hsv(0.78f, 0.7f, 1);
  for (int i = 0; i < RING_LEN && i < ram + 1; i++) {
    ringXY(RING_LEN - 1 - i, x, y);
    add(base_, x, y, violet, 0.08f * minf(1.0f, ram - i) * act);
  }

  // Running requests orbit bright and fast; queued ones dim, slow and backwards.
  int running = cur_.levels[L_RUNNING] > 6 ? 6 : cur_.levels[L_RUNNING];
  int queued = cur_.levels[L_QUEUED] > 6 ? 6 : cur_.levels[L_QUEUED];
  orbit_ += dt * (8.0f + 40.0f * lv_[L_TOK_S]);
  queueOrbit_ -= dt * 5.0f;
  if (orbit_ > 6000) orbit_ -= 6000;
  if (queueOrbit_ < -6000) queueOrbit_ += 6000;
  // Each running request is a bright head with a long tail fading out behind it, and headlights:
  // the next couple of cells fade up before the head reaches them, so even a slow chaser glides.
  constexpr int TAIL = 22;
  constexpr float LIGHTS = 2.5f;  // how far ahead the headlights reach, in cells
  FRGB head = primary(0);
  for (int i = 0; i < running; i++) {
    float p = orbit_ + i * (float)RING_LEN / running;
    int c0 = (int)floorf(p);
    float frac = p - c0;
    for (int j = -3; j <= TAIL; j++) {
      float d = frac + j;  // distance behind the head, in cells (negative: ahead of it)
      if (d < -LIGHTS || d > TAIL) continue;
      float k = d < 0 ? (1.0f + d / LIGHTS) * (1.0f + d / LIGHTS) : (1.0f - d / TAIL) * (1.0f - d / TAIL);
      ringXY(c0 - j + RING_LEN * 100, x, y);
      add(base_, x, y, head, 0.85f * k * act);
    }
  }
  for (int i = 0; i < queued; i++) {
    ringXY((int)(queueOrbit_ + i * (float)RING_LEN / queued) + RING_LEN * 100, x, y);
    add(acc_, x, y, accent(), 0.35f * act);
  }

  // Open connections to the engine: faint white dots drifting slowly round.
  int conns = cur_.levels[L_CONNECTIONS] > 8 ? 8 : cur_.levels[L_CONNECTIONS];
  connOrbit_ += dt * 1.5f;
  if (connOrbit_ > 6000) connOrbit_ -= 6000;
  for (int i = 0; i < conns; i++) {
    ringXY((int)(connOrbit_ + i * (float)RING_LEN / conns + 7), x, y);
    add(base_, x, y, {1, 1, 1}, 0.12f * act);
  }

  // A new request sends a comet once around the ring.
  for (Comet& c : comets_) {
    if (!c.on && pendStarts_ > 0) { c = {0, (float)RING_LEN, true}; pendStarts_--; }
  }
  for (Comet& c : comets_) {
    if (!c.on) continue;
    float p0 = c.pos;
    c.pos += 70.0f * dt;
    c.left -= 70.0f * dt;
    for (int i = (int)p0; i <= (int)c.pos; i++) {
      ringXY(i, x, y);
      add(acc_, x, y, accent(), act);
    }
    if (c.left <= 0) c.on = false;
  }

  // Interrupts make the ring crackle.
  float want = RING_LEN * 6.0f * lv_[L_INTERRUPTS] * lv_[L_INTERRUPTS] * dt;
  while (want > 0) {
    if (rnd() < want) {
      ringXY(irnd(RING_LEN), x, y);
      if (standbyMix_ > 0.3f) twinkle(x, y, hsv(0.6f, 0.2f, 1), 0.22f * act);
      else add(acc_, x, y, hsv(0.6f, 0.2f, 1), 0.22f * act);
    }
    want -= 1;
  }
}

void Renderer::drawLogic(float dt) {
  // The host's subsystems glow in from the inside edge of the ring, each owning a stretch of it
  // (clockwise from the top left): GPU, VRAM bandwidth, GPU power, CPU, disk, network, temperature.
  // A glow is brighter and reaches further in the busier its subsystem is; no pulsing.
  if (standbyMix_ > 0.9f) return;
  const float vis = 1.0f - 0.5f * standbyMix_;
  const bool showLevels = standbyMix_ < 0.5f;  // power and temperature never reach zero: busy only

  float rDisk = lv_[L_DISK_READ], wDisk = lv_[L_DISK_WRITE], rx = lv_[L_NET_RX], tx = lv_[L_NET_TX];
  float cpu = 0;
  for (int i = 0; i < N_CORES; i++) cpu += cores_[i];
  cpu /= N_CORES;
  float wShare = wDisk / (rDisk + wDisk + 1e-3f), txShare = tx / (rx + tx + 1e-3f);
  auto mix = [](FRGB a, FRGB b, float t) { return FRGB{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };

  // Network and disk traffic arrive in bursts, so the glows follow a much slower average than the
  // raw numbers: they swell over about half a second and ease off over about a second and a half.
  const float raw[7] = {lv_[L_GPU_BUSY], lv_[L_GPU_MEM_BUSY], showLevels ? lv_[L_GPU_POWER] : 0, cpu,
                        maxf(rDisk, wDisk), maxf(rx, tx), showLevels ? lv_[L_GPU_TEMP] : 0};
  float level[7];
  for (int b = 0; b < 7; b++) {
    float tau = raw[b] > glowLv_[b] ? 0.5f : 1.5f;
    glowLv_[b] += (raw[b] - glowLv_[b]) * (1.0f - expf(-dt / tau));
    level[b] = glowLv_[b];
  }
  float kShare = 1.0f - expf(-dt / 1.0f);
  glowShare_[0] += (wShare - glowShare_[0]) * kShare;
  glowShare_[1] += (txShare - glowShare_[1]) * kShare;
  wShare = glowShare_[0];
  txShare = glowShare_[1];
  const FRGB color[7] = {hsv(0.0f, 0.95f, 1), hsv(0.62f, 0.9f, 1), hsv(0.1f, 0.1f, 1), hsv(0.33f, 0.9f, 1),
                         mix(hsv(0.08f, 1, 1), hsv(0.0f, 1, 1), wShare),       // amber read, red write
                         mix(hsv(0.38f, 0.9f, 1), hsv(0.6f, 0.9f, 1), txShare),  // green in, blue out
                         hsv(0.62f * (1.0f - lv_[L_GPU_TEMP]), 0.9f, 1)};
  // The inside edge is the 52-cell loop just inside the ring; each cell takes the stretch of the
  // loop nearest it and how deep it sits from that edge.
  constexpr int LOOP = 52;
  for (int y = 1; y <= 14; y++)
    for (int x = 1; x <= 14; x++) {
      int dTop = y - 1, dRight = 14 - x, dBottom = 14 - y, dLeft = x - 1;
      int depth = dTop, edge = 0;
      if (dRight < depth) { depth = dRight; edge = 1; }
      if (dBottom < depth) { depth = dBottom; edge = 2; }
      if (dLeft < depth) { depth = dLeft; edge = 3; }
      float pos;  // position along the loop, clockwise from the top-left corner
      switch (edge) {
        case 0: pos = (float)(x - 1); break;
        case 1: pos = 13.0f + (y - 1); break;
        case 2: pos = 26.0f + (14 - x); break;
        default: pos = 39.0f + (14 - y); break;
      }
      FRGB sum = {0, 0, 0};
      for (int b = 0; b < 7; b++) {
        if (level[b] < 0.02f) continue;
        float center = (b + 0.5f) * LOOP / 7.0f, d = fabsf(pos - center);
        if (d > LOOP / 2.0f) d = LOOP - d;
        float half = LOOP / 14.0f;  // soft blend into the neighbours
        float across = d < half ? 1.0f : expf(-(d - half) * (d - half) / 3.0f);
        float reach = 0.5f + 3.0f * level[b];
        float k = level[b] * across * expf(-depth / reach);
        sum.r += color[b].r * k;
        sum.g += color[b].g * k;
        sum.b += color[b].b * k;
      }
      add(base_, x, y, sum, 0.45f * vis);
    }
}

void Renderer::drawStandby(float t) {
  // The wave only plays when the collector has gone quiet; an idle engine shows its own small
  // traffic on a dark panel instead.
  if (waveMix_ < 0.01f) return;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float v = 0.5f + 0.5f * sinf(t * 0.9f + x * 0.55f + 2.0f * sinf(y * 0.35f + t * 0.4f));
      float hue = t * 0.015f + (x + y) / 48.0f;
      add(base_, x, y, hsv(hue, 0.85f, 1), (0.10f * v * v * v + 0.015f) * waveMix_);
    }
  if (rnd() < 3.0f * (1.0f / 60.0f)) twinkle(irnd(W), irnd(H), {1, 1, 1}, 0.3f * waveMix_);
}

void Renderer::render(uint32_t nowMs, RGB out[W * H]) {
  if (startMs_ == 0 && lastMs_ == 0) { startMs_ = nowMs; lastMs_ = nowMs; }
  float dt = (nowMs - lastMs_) / 1000.0f;
  if (dt < 0) dt = 0;
  if (dt > 0.1f) dt = 0.1f;
  lastMs_ = nowMs;
  float t = (nowMs - startMs_) / 1000.0f;

  // No frames: full standby shimmer. Frames but no engine: the host's own activity keeps
  // playing (logic blocks, twinkle, ring crackle) over a faint shimmer.
  bool fresh = haveFrame_ && nowMs - lastFrameMs_ < STANDBY_AFTER_MS;
  // No frames: the wave alone. Engine idle or down: mostly the wave, with only real host activity
  // showing through. Reading or generating: everything.
  bool busy = cur_.state == STATE_READING || cur_.state == STATE_GENERATING || cur_.levels[L_RUNNING] > 0;
  float target = !fresh ? 1.0f : (busy ? 0.0f : 0.7f);
  standbyMix_ += (target - standbyMix_) * (1.0f - expf(-dt / 0.6f));
  waveMix_ += ((fresh ? 0.0f : 1.0f) - waveMix_) * (1.0f - expf(-dt / 0.6f));
  if (!fresh) { pendTokens_ = pendPrompt_ = pendDrafts_ = pendDisk_ = 0; pendStarts_ = pendEnds_ = 0; }

  updateInputs(dt);

  float fieldK = expf(-dt / FIELD_TAU), ringK = expf(-dt / RING_TAU);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float k = isRing(x, y) ? ringK : fieldK;
      acc_[y][x] = scale(acc_[y][x], k);
      base_[y][x] = {0, 0, 0};
    }

  drawField(dt, t);
  drawRing(dt);
  drawLogic(dt);
  drawStandby(t);
  drawTwinkles(dt);

  FRGB pix[H][W];
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      FRGB c = {acc_[y][x].r + base_[y][x].r, acc_[y][x].g + base_[y][x].g, acc_[y][x].b + base_[y][x].b};
      // keep runaway accumulation bounded so trails still fade
      acc_[y][x] = {minf(acc_[y][x].r, 1.5f), minf(acc_[y][x].g, 1.5f), minf(acc_[y][x].b, 1.5f)};
      pix[y][x] = {clamp01(c.r), clamp01(c.g), clamp01(c.b)};
    }
  if (photosafe_) limitFlashes(dt, t, pix);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      out[y * W + x] = {(uint8_t)(pix[y][x].r * 255 + 0.5f), (uint8_t)(pix[y][x].g * 255 + 0.5f),
                        (uint8_t)(pix[y][x].b * 255 + 0.5f)};
}

// Photosensitive safety (after WCAG 2.3.1 and the Harding test): a "flash" is a rise and fall in
// brightness of 10 % or more over an area big enough to matter (roughly a quarter of central vision),
// and more than 3 a second is the danger zone. Single sparks are too small to count; a whole patch
// of the panel brightening at once is what isn't fine. So the brightness of each 4x4 patch may only
// rise at FLASH_RISE per second (falls are free): a 10 % jump takes 0.4 s, keeping any patch under
// about 2 flashes a second. The patch gains are blended smoothly across the panel so no square
// edges show. Strict mode (setPhotosafe(true, true)) also stops each single LED flashing more than
// about 2.5 times a second, at the cost of a stuttery look.
void Renderer::limitFlashes(float dt, float t, FRGB (&pix)[H][W]) {
  constexpr float FLASH_RISE = 0.25f;  // display luminance (0..1) per second, per patch
  auto lum = [](FRGB c) {  // roughly what the LEDs emit after the firmware's gamma
    return 0.2126f * powf(c.r, 1.6f) + 0.7152f * powf(c.g, 1.6f) + 0.0722f * powf(c.b, 1.6f);
  };
  float gain[4][4];
  for (int py = 0; py < 4; py++)
    for (int px = 0; px < 4; px++) {
      float L = 0;
      for (int y = py * 4; y < py * 4 + 4; y++)
        for (int x = px * 4; x < px * 4 + 4; x++) L += lum(pix[y][x]);
      L /= 16;
      float& prev = patchLum_[py][px];
      float allowed = prev + FLASH_RISE * dt;
      gain[py][px] = 1.0f;
      if (L > allowed && L > 0) {
        gain[py][px] = powf(allowed / L, 1.0f / 1.6f);
        L = allowed;
      }
      prev = L;
    }
  // Each pixel takes its gain from the patch centres around it (bilinear), never more than its
  // own patch allows, so a limited patch fades into its neighbours instead of showing as a square.
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float gy = (y - 1.5f) / 4.0f, gx = (x - 1.5f) / 4.0f;
      int y0 = (int)floorf(gy), x0 = (int)floorf(gx);
      float fy = gy - y0, fx = gx - x0;
      auto g = [&](int yy, int xx) {
        yy = yy < 0 ? 0 : (yy > 3 ? 3 : yy);
        xx = xx < 0 ? 0 : (xx > 3 ? 3 : xx);
        return gain[yy][xx];
      };
      float k = (g(y0, x0) * (1 - fx) + g(y0, x0 + 1) * fx) * (1 - fy) +
                (g(y0 + 1, x0) * (1 - fx) + g(y0 + 1, x0 + 1) * fx) * fy;
      k = minf(k, gain[y / 4][x / 4]);
      if (k < 1.0f) pix[y][x] = scale(pix[y][x], k);
    }
  if (!strictFlash_) return;
  constexpr float PIXEL_GAP = 0.4f;    // seconds between flash-sized rises of one LED
  constexpr float PIXEL_STEP = 0.05f;  // held well under the 10 % flash threshold
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float L = lum(pix[y][x]), &low = pixLow_[y][x];
      if (L < low) low = L;
      if (L > low + PIXEL_STEP) {
        if (t - pixJump_[y][x] >= PIXEL_GAP) {
          pixJump_[y][x] = t;
          low = L;
        } else {
          pix[y][x] = scale(pix[y][x], powf((low + PIXEL_STEP) / L, 1.0f / 1.6f));
        }
      }
    }
}

}  // namespace blink
