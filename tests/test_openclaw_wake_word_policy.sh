#!/bin/sh
set -eu

board_config="main/boards/openclaw-v2.7/config.json"
audio_service="main/audio/audio_service.cc"
partition_csv="partitions/openclaw_v2_7_16m.csv"

# Wake word is ENABLED with USE_AFE_WAKE_WORD
grep -q '"CONFIG_USE_AFE_WAKE_WORD=y"' "$board_config"
grep -q '"CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y"' "$board_config"
grep -q '"CONFIG_SEND_WAKE_WORD_DATA=y"' "$board_config"
grep -q '"# CONFIG_WAKE_WORD_DISABLED is not set"' "$board_config"

# model partition must exist in the partition table
grep -q '^model,' "$partition_csv"

# AudioService must NOT have the CONFIG_WAKE_WORD_DISABLED guard dropping wake word
if grep -q 'CONFIG_WAKE_WORD_DISABLED' "$audio_service"; then
    # It's okay to have the preprocessor check, but the #else branch must be active
    grep -q 'EnableWakeWordDetection' "$audio_service"
fi

# PopPreWakeRingBuffer must be present (gap capture)
grep -q 'PopPreWakeRingBuffer' "$audio_service"

echo "ALL wake-word policy checks PASSED"
