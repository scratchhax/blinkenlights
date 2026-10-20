# Blinkenlights LLM display

A 16x16 WS2812B panel that flickers like Data's positronic brain or R2-D2's logic display,
driven by whatever llama.cpp or strata and the host are doing. Design notes are in the
project's design doc.

```
host/blinkd.py        collector: llama.cpp / strata + AMD GPUs + /proc -> 92-byte UDP frames, 20 Hz
host/blinkd.service   systemd unit
host/test_blinkd.py   tests against fake llama.cpp and strata servers
firmware/             ESP32 (PlatformIO + FastLED): receives frames over Wi-Fi, animates at 60 fps, OTA updates
firmware/lib/blink/   protocol + renderer, shared by the firmware and the preview
preview/              the same renderer in a terminal, plus a GIF maker
```

## Try it without hardware

```sh
cd preview && make && cd ..
python3 host/blinkd.py --simulate --stdout | preview/preview     # fake activity
python3 host/blinkd.py --stdout | preview/preview                # this machine, live
```

The preview needs a truecolor terminal. `--dump frames.rgb --seconds 20` plus
`python3 preview/to_gif.py frames.rgb panel.gif` makes a GIF (needs Pillow).

## Hardware

- ESP32 DevKit (any ESP32 with PlatformIO's `esp32dev` board works; change `board` otherwise).
- Panel DIN to GPIO 13 (D13) through a 74AHCT125 level shifter (it often works without one, but not always).
  Put a 330 ohm resistor in series with the data line.
- 5 V supply of 10 A or more, a 1000 uF capacitor across 5 V and GND at the panel,
  and power injected at both ends of the panel. Share GND between the supply and the ESP32.
- Firmware caps the draw at 2500 mA (`MAX_MILLIAMPS` in `firmware/src/config.h`).

If the picture is scrambled or mirrored, change `SERPENTINE`, `FLIP_X`, `FLIP_Y` or
`COLUMN_MAJOR` in `firmware/src/config.h`.

## Flash the ESP32

```sh
pip install platformio
cd firmware
cp src/secrets.example.h src/secrets.h    # fill in your Wi-Fi name and password
pio run -e usb -t upload                  # first time only, over USB
pio run -e ota -t upload                  # every time after that, over Wi-Fi
```

On Windows use `py -m platformio` in place of `pio`. Boards with a CP2102 USB chip need the
Silicon Labs "CP210x Universal Windows Driver" before a COM port appears. If the USB upload
says "Wrong boot mode" or stalls at "Connecting...", hold the board's BOOT button until it
starts writing. Over-the-air updates don't need the button.

The firmware is known to build with `espressif32@6.9.0` and `FastLED@3.7.8` (pinned in
`platformio.ini`); newer FastLED releases don't compile with that toolchain.

The panel joins Wi-Fi as `blinkpanel.local` and shows a slow shimmer until frames arrive.
During an over-the-air update the outer ring fills up as a progress bar. If you change
`OTA_PASSWORD` in `secrets.h`, change `--auth=` in `platformio.ini` to match.

## Run the collector on the LLM host (Ubuntu 22.04)

```sh
sudo apt install libnss-mdns       # lets the host resolve blinkpanel.local (Ubuntu Desktop has it)
python3 host/blinkd.py --udp blinkpanel.local --dump
```

`--dump` prints the normalized signals once a second, handy for checking that the GPUs,
disks and engine are seen. Then install `host/blinkd.service` to run it at boot.
If `.local` names don't resolve on your network, give the panel a fixed DHCP lease and use
its IP address instead.

- **llama.cpp:** start `llama-server` with `--metrics` to get queue depth and speculative
  decoding stats; `/slots` (on by default) gives the per-token sparks.
- **strata:** works out of the box. If you set an API key, pass it with `--api-key` or
  `BLINKD_API_KEY`.
- **USB instead of Wi-Fi:** `blinkd.py --serial /dev/ttyUSB0` (add yourself to the `dialout`
  group first). The firmware listens on both at once.
- Run blinkd as root (or as the user that runs the engine) so it can read the engine's
  per-thread CPU, which drives the background twinkle.
- Frames are tiny (92 bytes, 20 a second), and a lost one costs nothing: event counters
  carry over and the panel animates on its own between frames.

## Signals

About 30 levels plus per-core CPU, per-GPU load and per-thread engine CPU, read from:

- **llama.cpp** `/slots`, `/metrics`, `/health`
- **strata** `/metrics` (the JSON its Monitor tab reads), `/health`
- **AMD GPUs** `/sys/class/drm/card*/device`: `gpu_busy_percent`, `mem_busy_percent`,
  `mem_info_vram_*`, hwmon power and temperatures, `pp_dpm_sclk`, `pcie_bw`
- **Kernel** `/proc/stat`, `/proc/vmstat`, `/proc/diskstats`, `/proc/net/dev`, `/proc/meminfo`,
  `/proc/pressure/*`, `/proc/loadavg`, `/proc/net/tcp`, `/proc/<pid>/task/*/stat`, cpufreq

`firmware/lib/blink/blink_protocol.h` lists every level and counter in the frame.
