# AGENTS.md

Blinkenlights: a 16x16 WS2812B panel driven by LLM-engine + host telemetry.
Three parts share one wire protocol:
- `host/blinkd.py` — Python collector: reads llama.cpp/strata/DeepSeek/Maya + AMD GPUs + `/proc`, emits 92-byte frames at 20 Hz over UDP or USB serial.
- `firmware/` — ESP32 (PlatformIO + FastLED): parses frames, animates at 60 fps, OTA updates.
- `preview/` — the firmware renderer rebuilt on desktop (terminal + GIF), no hardware needed.

## The frame is the contract (most important)

`firmware/lib/blink/blink_protocol.h` defines the 92-byte frame. `host/blinkd.py` mirrors the
byte layout by hand (it does NOT read the header). The two MUST stay in sync.

- `blink_protocol.h` has `static_assert(sizeof(Frame)==92)`; `test_blinkd.py` asserts `len(f)==92`.
- Shared renderer code in `firmware/lib/blink/` is compiled by BOTH the firmware and `preview/` —
  changes there affect hardware and the desktop preview.
- Frame rate split: host sends ~20 Hz; renderer runs 60 fps; GIF dump runs 30 fps.
- New engine ids need no protocol change and can't break an old panel: the parser checks version
  and CRC but never the engine byte, so an unknown id reaches `primary()`'s `default:` and renders
  violet. The panel that shipped predates `ENGINE_MAYA`, so Maya is violet until it is reflashed.
- Event counters are 16-bit free-running and wrap at 65536; the panel diffs them, so a dropped
  UDP frame loses nothing. Don't encode deltas per-frame.

## Commands

Preview (no hardware, no build of the C++ needed beyond `make`):
```sh
cd preview && make && cd ..                       # builds preview/preview via Makefile
python3 host/blinkd.py --simulate --stdout | preview/preview    # fake activity
python3 host/blinkd.py --stdout | preview/preview               # this machine, live
```
Preview/GIF needs a truecolor terminal. GIF:
```sh
... | preview/preview --dump frames.rgb --seconds 20
python3 preview/to_gif.py frames.rgb panel.gif    # needs Pillow
python3 preview/flashcheck.py frames.rgb          # WCAG 2.3.1 / Harding flash safety check
```

Tests (pure Python stdlib + unittest; fake HTTP servers spin up in-process):
```sh
python3 host/test_blinkd.py                        # whole suite (verbose)
python3 host/test_blinkd.py EngineTests.test_strata   # single test
```
Tests read the real `/proc` of the machine, so `HostTests` can be flaky/slow on non-Linux.

Firmware (PlatformIO):
```sh
pip install platformio
cd firmware
cp src/secrets.example.h src/secrets.h             # fill Wi-Fi + OTA password
pio run -e usb -t upload                           # first flash only (USB)
pio run -e ota -t upload                           # every later flash (over Wi-Fi)
# XIAO ESP32-S3 board: -e s3 then -e s3-ota
```
On Windows use `py -m platformio` instead of `pio`.

### Reflashing the deployed panel (deferred, unresolved)

The panel at 192.168.121.7 (`blinkpanel`) predates `ENGINE_MAYA` and still needs one flash for
Maya's teal. Two blockers found investigating — resolve both before trying again:

- **Chip family is unconfirmed.** `platformio.ini` defaults `usb`/`ota` to `esp32dev`, but
  `s3-ota` pins this panel's exact IP and the status page reports ~261 KB free heap (an S3 tells,
  not a classic ESP32). A wrong-family flash boots to Wi-Fi and OTA again but paints the panel
  garbled until recovered over USB — which means unmounting it. Confirm via the silkscreen or the
  `esptool chip CPU id` line of a USB connect before choosing `ota` vs `s3-ota`.
- **OTA is not reachable right now.** Nothing answers UDP/3232 (this core's `ArduinoOTA::begin()`
  default, confirmed in `libraries/ArduinoOTA/src/ArduinoOTA.cpp`) — probed from both WSL and
  wheatley on the flat LAN, while the panel's `:80` status page and UDP/4210 frames are healthy.
  So `-e ota -t upload` cannot work as documented; the running sketch's OTA listener may be dead,
  or a firewall is in the way. Expect to need a USB session.

## Firmware gotchas

- Toolchain is PINNED in `platformio.ini`: `espressif32@6.9.0` + `FastLED@3.7.8`. Newer FastLED
  (3.10+) does NOT compile with this toolchain. Don't bump casually.
- `src/secrets.h` is required to build (a `#error` fires without it) and holds Wi-Fi SSID/password +
  `OTA_PASSWORD`. Its `OTA_PASSWORD` must match `--auth=` in `platformio.ini`'s OTA env upload_flags.
  Keep real credentials out of version control.
- Panel wiring / orientation is edited in `src/config.h`: `DATA_PIN` (default 13; XIAO overrides
  to GPIO 1 via `-DBOARD_DATA_PIN=1`), and `SERPENTINE`/`FLIP_X`/`FLIP_Y`/`COLUMN_MAJOR` if the
  picture is scrambled/mirrored. `MAX_MILLIAMPS` (2500) is the FastLED power cap.
- Build envs use `BOARD_DATA_PIN` (not `DATA_PIN`) as the build flag because FastLED uses
  `DATA_PIN` internally — don't rename it.

## Collector runtime notes

- `blinkd.py` runs on Linux (Ubuntu 22.04 target); relies on `/proc`, `/sys/class/drm` (AMD), cpufreq.
- Run as root (or as the engine's user) so it can read the engine's per-thread CPU. USB mode needs
  the user in the `dialout` group.
- `--dump` prints normalized signals once a second — use it to confirm GPUs/disks/engine are seen.
  Always pass `--url` when dumping: without it blinkd relies on discovery and reports `[none down]`.
- Engines run in Docker, so the host's listen socket belongs to `docker-proxy`, not the engine.
  `discover_engines()` matches the *listening* process's cmdline and therefore never finds them —
  `--url` (repeatable) is mandatory in practice, not a convenience. The poller follows whichever
  engine is actually busy. On wheatley: strata `:8085`, llama routers `:8083`/`:8084`, Maya `:8087`,
  DeepSeek `:8088`.
- Engine identity comes from `/health`: strata sends `service:"strata"`, Maya sends `version` with
  no `service`, llama.cpp sends neither. Missing the Maya case makes it fall through as llama.cpp.
  DeepSeek (DeepSeek-V4.1-Flash on strata 0.2.7, its own container) also sends `service:"strata"`
  and names its model (`"model": "deepseek-v4.1-flash"`); that prefix is what makes it
  `ENGINE_DEEPSEEK` (4) rather than strata. It shares strata's bearer key and `/metrics`. On the
  wire it is sent as strata (`WIRE_ENGINE` in `blinkd.py`): the firmware has no DeepSeek colour,
  and the shipped panel would render a new id violet.
- **strata 0.2 requires a bearer key on every endpoint but `/health`** — without one blinkd finds
  strata, then loops `lost ...: 401` and the panel goes dark. Set `BLINKD_API_KEY` (or `--api-key`);
  one key is sent to every engine, which is safe because engines that don't ask ignore the header.
  Only strata has a key today (read from the container's `API_KEY` env); Maya and llama.cpp don't.

### Adding an engine (the owner's rule: every new engine on wheatley lands here too)

Model Loader's engine switch gaining an engine means this app needs it as well, or the panel goes
dark or shows the wrong engine while it runs. In one change:
1. `blinkd.py`: an `ENGINE_*` id, its `_detect()` rule from `/health`, its name in the `names` map,
   a turn in the `Simulator` cycle.
2. Its colour. While the deployed panel is not reflashed (see "Reflashing the deployed panel"), map
   it in `WIRE_ENGINE` to the existing engine it looks most like, so it renders in a colour the
   panel knows instead of violet. Once a reflash is possible: an id in `blink_protocol.h`'s
   `enum Engine`, `primary()` / `accent()` colours in `blink_renderer.cpp` (and `hitKnown` if it is
   strata's server), and drop its `WIRE_ENGINE` entry.
3. `blinkd.service` and the port list above: its `--url`. Then on wheatley, the same `--url` in
   `/etc/systemd/system/blinkd.service`, `daemon-reload`, and a restart.
4. A `test_blinkd.py` case through the fake engine.

## Conventions

- No CI / lint / formatter config exists. Match surrounding style.
- Deployed on wheatley at `/usr/local/bin/blinkd.py` under `blinkd.service`; deploying means
  `scp host/blinkd.py jason@wheatley:` then `sudo cp` it there and `systemctl restart blinkd`.
- Protocol changes: edit `blink_protocol.h` AND the mirroring code in `blinkd.py` together, then
  run `python3 host/test_blinkd.py` and rebuild `preview/` to confirm the 92-byte layout holds.
