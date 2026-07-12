#!/bin/bash
# Build and run the host unit tests. No ESP-IDF or hardware needed.
# These cover the pure-logic modules (no esp_* / FreeRTOS dependencies).
set -e
cd "$(dirname "$0")"

SRC=../../components/podcast
OUT=build
mkdir -p "$OUT"

echo "=== building rss_parse host tests ==="
# Compile vendored yxml separately without -Wextra (it has unused-parameter
# warnings in its generated code that are not ours to fix).
gcc -std=c11 -g -w -I "$SRC" -c "$SRC/yxml.c" -o "$OUT/yxml.o"
gcc -std=c11 -Wall -Wextra -g \
    -I "$SRC" \
    -o "$OUT/test_rss_parse" \
    test_rss_parse.c "$SRC/rss_parse.c" "$OUT/yxml.o"

echo "=== running ==="
"$OUT/test_rss_parse"

echo "=== building quiet host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/ui/include \
    -o "$OUT/test_quiet" \
    test_quiet.c ../../components/ui/quiet.c

echo "=== running ==="
"$OUT/test_quiet"

echo "=== building usage host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/ui/include \
    -o "$OUT/test_usage" \
    test_usage.c ../../components/ui/usage.c

echo "=== running ==="
"$OUT/test_usage"

echo "=== building alarm_next host tests ==="
# config_store.h (included by alarm_next.h) pulls in esp_err.h and podcast.h
# just for a typedef and PODCAST_URL_MAX; stubs/ satisfies both without IDF.
gcc -std=c11 -Wall -Wextra -g \
    -I stubs \
    -I ../../components/ui/include \
    -I ../../components/config_store/include \
    -o "$OUT/test_alarm_next" \
    test_alarm_next.c ../../components/ui/alarm_next.c

echo "=== running ==="
"$OUT/test_alarm_next"

echo "=== building stats host tests ==="
# stats.c keeps its ESP persistence behind ESP_PLATFORM, so a plain gcc build
# compiles only the pure accumulation logic the test exercises.
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/stats/include \
    -o "$OUT/test_stats" \
    test_stats.c ../../components/stats/stats.c

echo "=== running ==="
"$OUT/test_stats"

echo "=== building rf_meta host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/source_stream \
    -o "$OUT/test_rf_meta" \
    test_rf_meta.c ../../components/source_stream/rf_meta.c

echo "=== running ==="
"$OUT/test_rf_meta"

echo "=== building pitch host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/tuner/include \
    -o "$OUT/test_pitch" \
    test_pitch.c ../../components/tuner/pitch.c -lm

echo "=== running ==="
"$OUT/test_pitch"

echo "=== building lang host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/ui/include \
    -o "$OUT/test_lang" \
    test_lang.c ../../components/ui/lang.c

echo "=== running ==="
"$OUT/test_lang"
