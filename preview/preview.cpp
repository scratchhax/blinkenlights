// preview.cpp - runs the panel's renderer on a desktop, so effects can be tuned without hardware.
//
//   python3 host/blinkd.py --stdout | preview/preview              # live in the terminal
//   python3 host/blinkd.py --simulate --stdout | preview/preview   # fake engine activity
//   preview/preview --udp 4210                                     # listen like the ESP32 does
//   ... | preview/preview --dump frames.rgb --seconds 15           # raw 30 fps frames for to_gif.py
//
// The terminal view needs a truecolor terminal; each LED is two half-block characters.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "blink_renderer.h"

static volatile bool g_stop = false;

static uint32_t nowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}

static void drawTerminal(const blink::RGB* px, const blink::FrameParser& parser, const blink::Frame& f) {
  static const char* engines[] = {"none", "llama.cpp", "strata", "Maya"};
  static const char* states[] = {"down", "idle", "reading", "generating"};
  printf("\x1b[H");
  for (int y = 0; y < blink::H; y += 2) {
    for (int x = 0; x < blink::W; x++) {
      const blink::RGB& a = px[y * blink::W + x];
      const blink::RGB& b = px[(y + 1) * blink::W + x];
      printf("\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm▀▀", a.r, a.g, a.b, b.r, b.g, b.b);
    }
    printf("\x1b[0m\n");
  }
  printf("\x1b[0m\x1b[K engine %-9s  state %-10s  frames %u  bad %u\n", engines[f.engine < 4 ? f.engine : 0],
         states[f.state < 4 ? f.state : 0], parser.good(), parser.bad());
  fflush(stdout);
}

int main(int argc, char** argv) {
  const char* dumpPath = nullptr;
  float seconds = 0;
  int udpPort = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpPath = argv[++i];
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = (float)atof(argv[++i]);
    else if (!strcmp(argv[i], "--udp") && i + 1 < argc) udpPort = atoi(argv[++i]);
    else {
      fprintf(stderr, "usage: preview [--udp PORT] [--dump FILE --seconds N]\n");
      return 2;
    }
  }
  signal(SIGINT, [](int) { g_stop = true; });

  int fd = 0;
  if (udpPort) {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)udpPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (sockaddr*)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

  FILE* dump = dumpPath ? fopen(dumpPath, "wb") : nullptr;
  if (dumpPath && !dump) { perror(dumpPath); return 1; }
  if (!dump) printf("\x1b[2J\x1b[?25l");

  blink::Renderer renderer;
  blink::FrameParser parser;
  blink::RGB px[blink::W * blink::H];
  renderer.begin(nowMs());

  const uint32_t start = nowMs();
  const uint32_t frameMs = dump ? 33 : 16;  // the dump is 30 fps for the GIF
  uint32_t next = start;
  bool eof = false;
  while (!g_stop) {
    pollfd p{fd, POLLIN, 0};
    int wait = (int)(next - nowMs());
    if (wait < 0) wait = 0;
    if (!eof && poll(&p, 1, wait) > 0 && (p.revents & (POLLIN | POLLHUP))) {
      uint8_t buf[4096];
      ssize_t n = read(fd, buf, sizeof buf);
      if (n == 0 && !udpPort) eof = true;
      for (ssize_t i = 0; i < n; i++)
        if (parser.feed(buf[i])) renderer.onFrame(parser.frame(), nowMs());
    } else if (eof) {
      usleep(wait * 1000);
    }
    uint32_t now = nowMs();
    if ((int32_t)(now - next) < 0) continue;
    next += frameMs;
    renderer.render(now, px);
    if (dump) fwrite(px, sizeof px, 1, dump);
    else drawTerminal(px, parser, parser.frame());
    if (seconds > 0 && now - start >= seconds * 1000) break;
  }
  if (dump) {
    fclose(dump);
    fprintf(stderr, "preview: %u frames received, %u bad\n", parser.good(), parser.bad());
  } else printf("\x1b[0m\x1b[?25h\n");
  return 0;
}
