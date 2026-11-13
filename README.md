# hdhomerun_scanner

A C scanner for SiliconDust HDHomeRun tuners that continuously scans the TV band and reports what it receives to [rabbitears.info](https://www.rabbitears.info/) for TV DX logging.

It is a port of the Perl `scan_tuner.pl` script, with added support for ATSC 3.0.

## Features

- Scans an RF channel range (or the tuner's channel map) and reports modulation, signal strength, SNR, symbol error quality, TSID and virtual channels
- **ATSC 3.0 support**: reports BSID, per-PLP info and L1 detail. If a station's service data is missing it retunes to all PLPs, then to only the PLPs that lock, so one marginal PLP doesn't hide the others
- Submits results in the same bzip2-compressed format as the Perl script, so rabbitears.info treats them identically
- Handles capture requests from the server: records the requested channel (TS for ATSC 1.0, ALP-PCAP or debug format for ATSC 3.0) and uploads it by FTP
- Runs unattended: network timeouts, clean shutdown on Ctrl-C or SIGTERM, and line-buffered output for systemd or log files
- Builds as a single fully static binary that runs on almost any x86-64 Linux

## Building

### 1. Get libhdhomerun

The scanner compiles SiliconDust's `libhdhomerun` directly into the binary. Clone it into this folder:

```bash
git clone https://github.com/Silicondust/libhdhomerun.git
```

### 2a. Static build (recommended)

Builds inside an Alpine Linux container with musl, giving a self-contained binary with no runtime dependencies. You need Docker (`sudo apt install docker.io` on Ubuntu/Debian).

```bash
./build-hdhrs-static.sh
```

The output is `./hdhomerun_scanner`. It's owned by root because Docker created it, so run `sudo chown $USER: hdhomerun_scanner` if you want to own it.

### 2b. Regular (dynamically linked) build

On Debian/Ubuntu:

```bash
sudo apt install build-essential libcurl4-openssl-dev libjson-c-dev libbz2-dev

gcc -Wall -O2 -std=gnu99 -D_GNU_SOURCE -I./libhdhomerun -I. \
    -o hdhomerun_scanner hdhomerun_scanner.c \
    libhdhomerun/hdhomerun_channels.c libhdhomerun/hdhomerun_channelscan.c \
    libhdhomerun/hdhomerun_control.c libhdhomerun/hdhomerun_debug.c \
    libhdhomerun/hdhomerun_device.c libhdhomerun/hdhomerun_device_selector.c \
    libhdhomerun/hdhomerun_discover.c libhdhomerun/hdhomerun_os_posix.c \
    libhdhomerun/hdhomerun_pkt.c libhdhomerun/hdhomerun_sock.c \
    libhdhomerun/hdhomerun_sock_posix.c libhdhomerun/hdhomerun_sock_netlink.c \
    libhdhomerun/hdhomerun_video.c \
    -lcurl -ljson-c -lbz2 -lm -lpthread
```

## Configuration

Settings are read from `scanner.conf` in the current directory, or from the file given with `-c`. The included `scanner.conf` documents every option. The most important ones:

| Setting | Meaning |
|---|---|
| `tuner_id` | 8-digit hex device ID, or `FFFFFFFF` for the first one found |
| `tuner_index` | Which tuner on the device to use (0, 1, …) |
| `min_channel` / `max_channel` | RF channel range to scan |
| `seek_timeout` | Seconds to wait for a lock on each channel |
| `psip_wait_time` / `atsc3_psip_wait_time` | Seconds to wait for PSIP (ATSC 1.0) or SLT (ATSC 3.0) data after lock |
| `retry_delay` | Seconds between complete scans |
| `override_channel` | `CH,CALLSIGN`: per-channel callsign override (can be repeated) |

Run `hdhomerun_config discover` to find your device ID.

## Usage

```
./hdhomerun_scanner [options]

  -c, --config FILE    Configuration file (default: scanner.conf)
  -d, --debug          Enable debug output
  -t, --tuner ID       Tuner ID (overrides config)
  -i, --index NUM      Tuner index (overrides config)
  -u, --url URL        Submit URL (overrides config)
  -o, --override LIST  Per-channel callsign overrides, e.g. -o 26,W26EV,18,W18EG
  -h, --help           Show help

Testing:
  --test-capture CHAN  Record a capture from a channel, upload it, then exit
  --test-duration SECS Duration of the test capture (default 30)
  --test-upload FILE   Upload a local file by FTP, then exit
```

The scanner runs until stopped with Ctrl-C.

## A note on the output format

The rabbitears.info server expects submissions to look exactly like the Perl script's. Some things that look odd are deliberate and shouldn't be "fixed": numeric fields sent as strings, the `libwww-perl` User-Agent, and the 5-byte `memBzip` header before the bzip2 data.

## Credits

- Based on the Perl `scan_tuner.pl` by kb8u
- `libhdhomerun` © Silicondust USA Inc., distributed under the GNU LGPL

## License

Copyright © 2026 Mark J. Colombo

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE).
