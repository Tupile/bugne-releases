# Source-level checks for the playlist feature (ui.c and web_config.c cannot be
# linked on a host). They pin the contract, not the behaviour: a refactor that
# drops one of these guards fails here on purpose.
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
ui = (ROOT / "components/ui/ui.c").read_text()
web = (ROOT / "components/web_config/web_config.c").read_text()


def ui_body(signature):
    m = re.search(r"^" + re.escape(signature) + r"\n\{(.*?)^\}", ui, re.M | re.S)
    assert m, signature
    return m.group(1)


def web_body(name):
    m = re.search(r"^static esp_err_t " + name + r"\(httpd_req_t \*req\)\n\{(.*?)^\}", web, re.M | re.S)
    assert m, name
    return m.group(1)


# GATE PLACEMENT RULE: the row tap is a user entry point, the parental gate is
# its first statement.
tap = ui_body("static void on_playlist_track(lv_event_t *e)")
assert tap.lstrip().startswith("if (play_denied()) return;"), "tap gate"

# The web play action is gated before it touches the playing snapshot.
m = re.search(r"case UI_REMOTE_PLAY_PLAYLIST:(.*?)case UI_REMOTE_SLEEP", ui, re.S)
assert m, "remote case"
assert m.group(1).index("play_denied()") < m.group(1).index("playlist_start("), "remote gate"

# One place where a playlist becomes the play context, used by the tap and by
# the web action.
assert ui.count("static void playlist_play(size_t count, int start)\n{") == 1
assert "playlist_play(" in tap
# The tap plays what the screen shows: no SD access (no file read, no stat).
assert "playlist_read(" not in tap and "playlist_start(" not in tap and "stat(" not in tap

# Playback reuses the library context: no new play context to keep in sync
# with next/prev, auto-advance, statistics and /api/playback.
assert "PLAY_CTX_PLAYLIST" not in ui
start = ui_body("static bool playlist_start(const char *name, int file_index)")
assert "playlist_play(" in start
play = ui_body("static void playlist_play(size_t count, int start)")
assert "PLAY_CTX_LIBRARY" in play and "play_ctx_at(" in play
# A failed read must not empty the snapshot of an album that is playing.
assert "n > 0 && s_play_ctx == PLAY_CTX_LIBRARY" in start

# Both screens are reachable by the debug nav.
assert '{ "playlists",' in ui and '{ "playlist_tracks",' in ui

# Web: the body route rejects with ESP_FAIL before reading (httpd would
# otherwise drain the declared body on its single task); both handlers auth.
post = web_body("playlist_post")
assert "REQUIRE_AUTH(req, ESP_FAIL)" in post
assert "REQUIRE_AUTH(req, ESP_OK)" in web_body("playlist_get")
size = post.index("PLAYLIST_BODY_MAX")
assert "return ESP_FAIL" in post[size:size + 400], "413 closes the connection"
# Names are validated after URL decoding, by the shared rule.
assert "playlist_name_valid(" in post and "playlist_name_valid(" in web_body("playlist_get")
# Every path of a body goes through the SD guard, in the one helper the POST
# handler uses to read the tracks.
helper = re.search(r"^static bool playlist_tracks_from_json\(.*?^\}", web, re.M | re.S).group(0)
assert "source_sd_rel_path_safe(" in helper and "playlist_path_normalize(" in helper
assert "playlist_tracks_from_json(" in post
# One URI, GET and POST: the handler budget is tight.
assert web.count('.uri = "/api/playlist"') == 2

print("playlist rules (gates, snapshot reuse, web auth and bounds): passed")
