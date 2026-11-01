#!/bin/sh
# Кассета — музыкальный плеер для NextUI (TrimUI Brick / Smart Pro)
PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
export PAK_DIR
cd "$PAK_DIR" || exit 1
chmod +x ./kasseta.elf 2>/dev/null
LOG="${LOGS_PATH:-$PAK_DIR}/Kasseta.txt"
mkdir -p "$(dirname "$LOG")"
./kasseta.elf > "$LOG" 2>&1
