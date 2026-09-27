#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/tools/intro/Dm01Intro.h"

for p in pomodoro 3d_cube neko flappy btc_ticker pomodoro_cyd 3d_cube_cyd neko_cyd flappy_cyd btc_ticker_cyd \
         pong_clock pong_clock_cyd github_squares github_squares_cyd asteroids; do
  [ -f "$ROOT/$p/$p.ino" ] || continue
  cp "$SRC" "$ROOT/$p/Dm01Intro.h"
  echo "-> $p/Dm01Intro.h"
done
