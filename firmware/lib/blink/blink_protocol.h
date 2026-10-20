// blink_protocol.h - the signal frame the host collector (blinkd) sends to the panel.
//
// One frame is a snapshot of every signal, normalized to 0..255, plus free-running
// 16-bit event counters. The panel diffs counters between frames, so a dropped frame
// loses no token sparks. Frames arrive about 20 times a second over USB serial or UDP;
// the panel animates on its own at 60 fps in between.
//
// Wire format (little-endian, 92 bytes):
//   'B' 'L' version seq engine state n_gpus brightness
//   levels[32] cores[16] gpus[8] threads[14] counters[6] (u16) crc16 (u16)
// crc16 is CRC-16/CCITT-FALSE over every byte before it.
//
// host/blinkd.py mirrors this layout; keep the two in step.
#pragma once
#include <stdint.h>
#include <string.h>

namespace blink {

constexpr uint8_t MAGIC0 = 'B';
constexpr uint8_t MAGIC1 = 'L';
constexpr uint8_t VERSION = 1;

enum Engine : uint8_t { ENGINE_NONE = 0, ENGINE_LLAMA = 1, ENGINE_STRATA = 2, ENGINE_MAYA = 3 };
enum State : uint8_t { STATE_DOWN = 0, STATE_IDLE = 1, STATE_READING = 2, STATE_GENERATING = 3 };

// levels[]: 0..255 unless noted
enum Level : uint8_t {
  L_GPU_BUSY = 0,      // mean GPU load
  L_GPU_MEM_BUSY,      // GPU memory controller load
  L_VRAM,              // VRAM used / total
  L_GPU_POWER,         // GPU power / power cap
  L_GPU_TEMP,          // hottest GPU, 30..95 C
  L_GPU_CLOCK,         // shader clock / max
  L_SWAP,              // swap in + out traffic
  L_CPU_IOWAIT,
  L_CPU_IRQ,           // irq + softirq share
  L_MEM,               // RAM used
  L_PSI_MEM,           // pressure stall, some avg10
  L_PSI_IO,
  L_PSI_CPU,
  L_DISK_READ,         // log scale up to ~7 GB/s
  L_DISK_WRITE,
  L_NET_RX,            // log scale up to ~1.25 GB/s
  L_NET_TX,
  L_CTX_SWITCH,        // context switches per second, log scale
  L_INTERRUPTS,        // interrupts per second, log scale
  L_MAJ_FAULTS,        // major page faults per second, log scale
  L_TOK_S,             // generation speed
  L_PREFILL,           // prompt read progress of the running request
  L_CONTEXT,           // context window fill of the busiest slot
  L_EXPERT_HIT,        // strata expert cache hit rate (255 when unknown)
  L_QUEUED,            // RAW COUNT of waiting requests
  L_RUNNING,           // RAW COUNT of running requests
  L_PCIE,              // PCIe traffic
  L_PAGING,            // page-cache churn: KB paged in/out plus minor faults
  L_LOADAVG,           // load1 / cores
  L_CPU_FREQ,          // mean core clock / max
  L_SPEC_ACCEPT,       // speculative draft acceptance rate
  L_CONNECTIONS,       // RAW COUNT of TCP connections to the engine port
  L_COUNT = 32
};

// counters[]: free-running, wrap at 65536
enum Counter : uint8_t {
  C_TOKENS = 0,        // tokens generated
  C_PROMPT_TOKENS,     // prompt tokens read, in units of 8 tokens
  C_REQ_START,         // requests started
  C_REQ_END,           // requests finished
  C_DRAFTS_ACCEPTED,   // speculative draft tokens accepted
  C_DISK_BURSTS,       // big reads: strata SSD expert loads, or major-fault bursts
  C_COUNT = 6
};

constexpr int N_CORES = 16;
constexpr int N_GPUS = 8;
constexpr int N_THREADS = 14;

#pragma pack(push, 1)
struct Frame {
  uint8_t magic[2];
  uint8_t version;
  uint8_t seq;
  uint8_t engine;
  uint8_t state;
  uint8_t n_gpus;
  uint8_t brightness;          // overall brightness the host wants, 1..255; 0 = the panel decides
  uint8_t levels[L_COUNT];
  uint8_t cores[N_CORES];      // per-core (or per core group) load
  uint8_t gpus[N_GPUS];        // per-GPU load, first n_gpus valid
  uint8_t threads[N_THREADS];  // engine worker threads' CPU, folded into 14 columns
  uint16_t counters[C_COUNT];
  uint16_t crc;
};
#pragma pack(pop)

static_assert(sizeof(Frame) == 92, "frame layout changed; update blinkd.py too");

inline uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// Byte-at-a-time parser for a stream (serial) or a datagram (UDP: feed every byte).
// Resynchronizes on the magic bytes after noise or a dropped byte.
class FrameParser {
 public:
  // Returns true when a complete, valid frame is ready in frame().
  bool feed(uint8_t b) {
    if (len_ == 0 && b != MAGIC0) return false;
    if (len_ == 1 && b != MAGIC1) { len_ = (b == MAGIC0) ? 1 : 0; return false; }
    buf_[len_++] = b;
    if (len_ < sizeof(Frame)) return false;
    Frame f;
    memcpy(&f, buf_, sizeof f);
    if (f.version != VERSION || crc16(buf_, sizeof(Frame) - 2) != f.crc) {
      bad_++;
      resync();
      return false;
    }
    len_ = 0;
    frame_ = f;
    good_++;
    return true;
  }
  const Frame& frame() const { return frame_; }
  uint32_t good() const { return good_; }
  uint32_t bad() const { return bad_; }

 private:
  // A bad frame may have started on a false 'B' 'L' inside another frame's data:
  // keep everything from the next magic onward instead of throwing it away.
  void resync() {
    for (size_t i = 1; i + 1 < sizeof(Frame); i++) {
      if (buf_[i] == MAGIC0 && buf_[i + 1] == MAGIC1) {
        len_ = sizeof(Frame) - i;
        memmove(buf_, buf_ + i, len_);
        return;
      }
    }
    len_ = (buf_[sizeof(Frame) - 1] == MAGIC0) ? 1 : 0;
    if (len_) buf_[0] = MAGIC0;
  }

  uint8_t buf_[sizeof(Frame)];
  size_t len_ = 0;
  Frame frame_{};
  uint32_t good_ = 0, bad_ = 0;
};

}  // namespace blink
