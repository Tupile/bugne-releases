from pathlib import Path
import subprocess
import tempfile
import re

ROOT = Path(__file__).resolve().parents[2]
UI = (ROOT / "components/ui/ui.c").read_text()


def function(text, name):
    match = re.search(r"^.*\b" + name + r"\([^;]*?\)\n\{", text, re.M)
    assert match, name
    start = match.start()
    opening = text.index("{", match.end() - 1)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define taskENTER_CRITICAL(m) ((void)0)
#define taskEXIT_CRITICAL(m) ((void)0)
#define PLAY_CTX_NONE 0
#define PLAY_CTX_LIBRARY 3
static uint32_t s_play_generation = 1, s_advance_generation, s_refresh_generation;
static uint32_t s_refresh_owner_generation;
static bool s_stop_requested, s_advance, s_play_retrying;
static bool s_play_failed, s_play_failed_local;
static bool s_refresh_ok, s_refreshing, s_refresh_done;
static volatile bool s_refresh_cancel;
typedef struct { uint32_t generation; int play_ctx; bool is_file; } play_req_t;
'''
helpers = "\n".join(function(UI, name) for name in (
    "play_current", "refresh_cancel", "refresh_begin", "refresh_finished",
    "play_cancel", "play_ended", "play_retrying",
))
cases = r'''
int main(void)
{
    play_req_t req = {.generation = 1, .play_ctx = PLAY_CTX_LIBRARY, .is_file = true};
    assert(play_current(req.generation));
    play_ended(&req, true, false);
    assert(s_advance && s_advance_generation == 1);
    play_cancel();
    assert(!play_current(req.generation) && !s_advance);
    play_ended(&req, true, false);
    play_retrying(req.generation, true);
    assert(!s_advance && !s_play_retrying);
    s_stop_requested = false;
    req.generation = s_play_generation;
    play_ended(&req, false, true);
    assert(!s_advance && s_play_failed && s_play_failed_local);
    uint32_t queued = s_refresh_generation;
    refresh_cancel();
    assert(!refresh_begin(queued) && s_refresh_cancel);
    queued = s_refresh_generation;
    s_refresh_owner_generation = queued;
    s_refreshing = true;
    assert(refresh_begin(queued) && !s_refresh_cancel);
    refresh_cancel();
    assert(!refresh_begin(queued) && s_refresh_cancel);
    refresh_finished(queued, true);
    assert(s_refresh_done && !s_refresh_ok && !s_refreshing);
    s_refresh_owner_generation++;
    s_refreshing = true;
    s_refresh_done = false;
    refresh_finished(queued, true);
    assert(s_refreshing && !s_refresh_done);
    puts("review playback cancellation helpers: passed");
}
'''
with tempfile.TemporaryDirectory(prefix="bugne-review-playback-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(prelude + helpers + cases)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
