#!/bin/bash
# build-static.sh — build a fully static hdhomerun_scanner using Alpine + musl
set -e

docker run --rm -v "$PWD:/src" -w /src alpine:3.19 sh -c '
  set -e

  echo "Installing build dependencies..."
  apk add --no-cache \
    gcc musl-dev make linux-headers \
    curl-dev curl-static \
    json-c-dev \
    bzip2-dev bzip2-static \
    openssl-dev openssl-libs-static \
    zlib-dev zlib-static \
    nghttp2-dev nghttp2-static \
    brotli-dev brotli-static \
    zstd-dev zstd-static \
    c-ares-dev c-ares-static \
    libidn2-dev libidn2-static \
    libunistring-dev libunistring-static \
    libpsl-dev libpsl-static

  echo "Compiling..."
  gcc -Wall -O2 -std=gnu99 -D_GNU_SOURCE \
      -Wno-int-conversion -Wno-stringop-truncation -Wno-unused-parameter \
      -I./libhdhomerun -I. \
      -static \
      -o hdhomerun_scanner \
      hdhomerun_scanner.c \
      libhdhomerun/hdhomerun_channels.c \
      libhdhomerun/hdhomerun_channelscan.c \
      libhdhomerun/hdhomerun_control.c \
      libhdhomerun/hdhomerun_debug.c \
      libhdhomerun/hdhomerun_device.c \
      libhdhomerun/hdhomerun_device_selector.c \
      libhdhomerun/hdhomerun_discover.c \
      libhdhomerun/hdhomerun_os_posix.c \
      libhdhomerun/hdhomerun_pkt.c \
      libhdhomerun/hdhomerun_sock.c \
      libhdhomerun/hdhomerun_sock_posix.c \
      libhdhomerun/hdhomerun_sock_netlink.c \
      libhdhomerun/hdhomerun_video.c \
      -lcurl -lnghttp2 -lssl -lcrypto -lbrotlidec -lbrotlicommon \
      -lzstd -lz -lcares -lidn2 -lunistring -lpsl \
      -ljson-c -lbz2 -lm -lpthread

  echo ""
  echo "Build successful. Binary info:"
  file hdhomerun_scanner
  ls -lh hdhomerun_scanner
  echo ""
  echo "Verifying no dynamic dependencies:"
  ldd hdhomerun_scanner || true
'
