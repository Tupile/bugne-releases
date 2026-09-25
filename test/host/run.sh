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

echo "=== building sd_path host tests ==="
# The real source_sd.h comes first so the podcast stub of the same name in
# stubs/ does not shadow it; stubs/ only provides esp_err.h.
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/source_sd/include -I stubs \
    -o "$OUT/test_sd_path" \
    test_sd_path.c

echo "=== running ==="
"$OUT/test_sd_path"

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

echo "=== building sleep_fade host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/ui/include \
    -o "$OUT/test_sleep_fade" \
    test_sleep_fade.c ../../components/ui/sleep_fade.c

echo "=== running ==="
"$OUT/test_sleep_fade"

echo "=== building tone host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/ui/include \
    -o "$OUT/test_tone" \
    test_tone.c ../../components/ui/tone.c -lm

echo "=== running ==="
"$OUT/test_tone"

echo "=== building epmeta host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/ui/include \
    -I ../../components/decode/include -I ../../components/decode \
    -o "$OUT/test_epmeta" \
    test_epmeta.c ../../components/ui/epmeta.c ../../components/decode/tags.c

echo "=== running ==="
"$OUT/test_epmeta"

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

echo "=== building memo host tests ==="
# memo.h pulls esp_err.h only for the memo_send declaration; stubs/ covers it.
gcc -std=c11 -Wall -Wextra -g \
    -I stubs \
    -I ../../components/memo/include \
    -o "$OUT/test_memo_wav" \
    test_memo_wav.c ../../components/memo/memo_wav.c
gcc -std=c11 -Wall -Wextra -g \
    -I stubs \
    -I ../../components/memo/include \
    -o "$OUT/test_memo_name" \
    test_memo_name.c ../../components/memo/memo_name.c

echo "=== running ==="
"$OUT/test_memo_wav"
"$OUT/test_memo_name"

echo "=== building memo_store host tests ==="
# memo_store.c scans a real directory: MEMO_ABS_DIR is redirected to /tmp
# through the #ifndef seam in memo.h. _DEFAULT_SOURCE exposes glibc strlcpy.
gcc -std=c11 -Wall -Wextra -g -D_DEFAULT_SOURCE \
    -DMEMO_ABS_DIR='"/tmp/bugne-memo-test"' \
    -I stubs \
    -I ../../components/memo/include \
    -o "$OUT/test_memo_store" \
    test_memo_store.c ../../components/memo/memo_store.c ../../components/memo/memo_name.c

echo "=== building memo_send host tests ==="
# Single-TU build: the test includes memo_send.c to share the scripted
# esp_http_client stub state.
gcc -std=c11 -Wall -Wextra -g \
    -I stubs \
    -I ../../components/memo/include \
    -o "$OUT/test_memo_send" \
    test_memo_send.c

echo "=== running ==="
"$OUT/test_memo_store"
"$OUT/test_memo_send"

echo "=== building played host tests ==="
# The storage paths are redirected to /tmp through the #ifndef seam.
gcc -std=c11 -Wall -Wextra -g \
    -DPLAYED_DIR='"/tmp/bugne-played-test"' \
    -I stubs \
    -I ../../components/podcast/include \
    -o "$OUT/test_played" \
    test_played.c ../../components/podcast/played.c

echo "=== running ==="
"$OUT/test_played"

echo "=== building podcast_resume host tests ==="
gcc -std=c11 -Wall -Wextra -g \
    -DPODCAST_RESUME_DIR='"/tmp/bugne-resume-test"' \
    -I stubs \
    -I ../../components/podcast/include \
    -o "$OUT/test_podcast_resume" \
    test_podcast_resume.c ../../components/podcast/podcast_resume.c

echo "=== running ==="
"$OUT/test_podcast_resume"

echo "=== building logstore host tests ==="
# Single-TU build: the test includes logstore.c to drive its vprintf hook.
gcc -std=c11 -Wall -Wextra -g \
    -I stubs \
    -I ../../components/logstore/include \
    -o "$OUT/test_logstore" \
    test_logstore.c

echo "=== running ==="
"$OUT/test_logstore"

echo "=== building tags host tests ==="
# tags.c is pure (no ESP dependency), so it compiles straight into the test.
gcc -std=c11 -Wall -Wextra -g \
    -I ../../components/decode/include \
    -o "$OUT/test_tags" \
    test_tags.c ../../components/decode/tags.c

echo "=== running ==="
"$OUT/test_tags"

echo "=== checking config field parity ==="
# Not a C test: compares the fields config_store parses against the fields it
# writes back, which is the exact class that dropped the `quiet` array once.
python3 check_config_parity.py

echo "=== building review memo fault tests ==="
# Same memo_store.c as above, but with rename()/fopen() redirected through the
# test's own wrappers so the collision and rename-failure branches of
# reserve_part() are exercised (they cannot be provoked on a real filesystem).
gcc -std=c11 -Wall -Wextra -g -D_DEFAULT_SOURCE \
    -DMEMO_ABS_DIR='"/tmp/bugne-memo-fault-test"' \
    -I stubs \
    -I ../../components/memo/include \
    -o "$OUT/test_review_memo_faults" \
    test_review_memo_faults.c ../../components/memo/memo_name.c

echo "=== running ==="
"$OUT/test_review_memo_faults"

echo "=== building podcast storage/cancellation tests ==="
# podcast.c is included directly by the test, which redirects every stdio and
# dirent call plus esp_http_client so the storage faults, the MP3 trim, the
# cache scan and every cancellation point can be driven deterministically.
# podcast_lot23_stubs/ provides the ESP headers; stubs/podcast.h must NOT be on
# the include path here (it would shadow the real podcast.h).
# cJSON comes from the registry since ESP-IDF 6 (espressif/cjson), fetched into
# managed_components/ by the first idf.py reconfigure or build.
CJSON="../../managed_components/espressif__cjson/cJSON"
[ -f "$CJSON/cJSON.c" ] || { echo "missing $CJSON: run idf.py reconfigure first"; exit 1; }
LOT23_INC="$OUT/lot23_inc"
rm -rf "$LOT23_INC"; mkdir -p "$LOT23_INC"
for f in stubs/*; do
    [ "$(basename "$f")" = "podcast.h" ] && continue
    cp -r "$f" "$LOT23_INC/"
done
gcc -std=c11 -Wall -Wextra -g \
    -I podcast_lot23_stubs \
    -I "$LOT23_INC" \
    -I ../../components/podcast \
    -I ../../components/podcast/include \
    -I "$CJSON" \
    -o "$OUT/test_podcast_lot23" \
    test_podcast_lot23.c ../../components/podcast/rss_parse.c "$OUT/yxml.o" "$CJSON/cJSON.c"

echo "=== running ==="
"$OUT/test_podcast_lot23"

echo "=== running source-level review tests ==="
# Not C tests of their own: each one extracts the functions it covers from the
# real source with a regex, compiles them against a scripted harness, and
# asserts on the behaviour. They cover the code that cannot be linked on a host
# (ui.c's worker, net.c's event handlers, config_store's transactions) and they
# break on purpose when those functions are refactored.
for t in test_review_playback_worker.py \
         test_review_playback_cancel.py \
         test_review_source_startup.py \
         test_review_ui_save_feedback.py \
         test_review_web_body_rules.py \
         test_review_net_task_faults.py \
         test_review_ha_tls.py \
         test_lot4_config_transactions.py; do
    echo "--- $t"
    python3 "$t"
done

echo "=== running web page tests ==="
# Parses the embedded page, syntax-checks every inline script and inline event
# handler, then drives the save/install handlers against a scripted fetch.
node test_lot4_web_actions.js
node test_web_voice.js
