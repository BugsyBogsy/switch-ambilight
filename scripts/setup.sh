#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT/ref"
if [[ ! -f "$ROOT/ref/Atmosphere/stratosphere/Makefile" ]]; then
    if [[ -e "$ROOT/ref/Atmosphere" ]]; then echo 'ref/Atmosphere exists but is incomplete; check/remove it manually.' >&2; exit 1; fi
    git clone --branch 1.12.1 --depth 1 --recursive https://github.com/Atmosphere-NX/Atmosphere.git "$ROOT/ref/Atmosphere"
fi
if [[ ! -f "$ROOT/ref/libtesla/include/tesla.hpp" ]]; then
    if [[ -e "$ROOT/ref/libtesla" ]]; then echo 'ref/libtesla exists but is incomplete; check/remove it manually.' >&2; exit 1; fi
    git clone --depth 1 https://github.com/WerWolv/libtesla.git "$ROOT/ref/libtesla"
fi
printf '\nAtmosphere: '
git -C "$ROOT/ref/Atmosphere" describe --tags --always
printf 'libtesla: '
git -C "$ROOT/ref/libtesla" rev-parse --short HEAD
