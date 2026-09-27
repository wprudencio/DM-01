#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLI="${ARDUINO_CLI:-$ROOT/bin/arduino-cli}"
BUILD_DIR="$ROOT/firmware-build"
OUT_DIR="$ROOT/webui/public/firmware"

C3_FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc"
CYD_FQBN="esp32:esp32:esp32:UploadSpeed=460800"

# variant sketch fqbn [project]
TARGETS=(
  "c3  pomodoro      $C3_FQBN"
  "c3  3d_cube       $C3_FQBN"
  "c3  neko          $C3_FQBN"
  "c3  flappy        $C3_FQBN"
  "c3  btc_ticker    $C3_FQBN"
  "c3  pong_clock      $C3_FQBN"
  "c3  github_squares  $C3_FQBN"
  "c3  asteroids       $C3_FQBN"
  "c3  dvd             $C3_FQBN"
  "c3  hn_display      $C3_FQBN hn"
  "cyd pomodoro_cyd  $CYD_FQBN"
  "cyd 3d_cube_cyd   $CYD_FQBN"
  "cyd neko_cyd      $CYD_FQBN"
  "cyd flappy_cyd    $CYD_FQBN"
  "cyd btc_ticker_cyd $CYD_FQBN"
  "cyd pong_clock_cyd    $CYD_FQBN"
  "cyd github_squares_cyd $CYD_FQBN"
  "cyd asteroids_cyd $CYD_FQBN"
  "cyd dvd_cyd       $CYD_FQBN"
  "cyd hn_cyd        $CYD_FQBN hn"

  # QUARTZ light-LCD editions (the /quartz site)
  "c3  pomodoro_quartz       $C3_FQBN"
  "c3  3d_cube_quartz        $C3_FQBN"
  "c3  flappy_quartz         $C3_FQBN"
  "c3  pong_clock_quartz     $C3_FQBN"
  "c3  btc_ticker_quartz     $C3_FQBN"
  "c3  dvd_quartz            $C3_FQBN"
  "c3  asteroids_quartz      $C3_FQBN"
  "c3  hn_quartz             $C3_FQBN"
  "c3  github_squares_quartz $C3_FQBN"
  "c3  neko_quartz         $C3_FQBN"
  "cyd pomodoro_quartz_cyd   $CYD_FQBN"
  "cyd flappy_quartz_cyd     $CYD_FQBN"
  "cyd pong_clock_quartz_cyd $CYD_FQBN"
  "cyd neko_quartz_cyd       $CYD_FQBN"
  "cyd btc_ticker_quartz_cyd $CYD_FQBN"
  "cyd dvd_quartz_cyd        $CYD_FQBN"
  "cyd asteroids_quartz_cyd  $CYD_FQBN"
  "cyd hn_quartz_cyd        $CYD_FQBN"
  "cyd github_squares_quartz_cyd $CYD_FQBN"
  "cyd 3d_cube_quartz_cyd    $CYD_FQBN"
)

for target in "${TARGETS[@]}"; do
  read -r variant sketch fqbn project <<<"$target"
  project="${project:-${sketch%_cyd}}"
  echo "==> compiling $sketch ($variant)"
  "$CLI" compile --fqbn "$fqbn" --output-dir "$BUILD_DIR/$sketch" "$ROOT/$sketch"
  mkdir -p "$OUT_DIR/$project"
  cp "$BUILD_DIR/$sketch/$sketch.ino.merged.bin" "$OUT_DIR/$project/$variant.bin"
  echo "    -> public/firmware/$project/$variant.bin"
done

echo "firmware ready: ${TARGETS[*]}"
