# Source-level checks for the 2026-09-23 web handler rules (web_config.c cannot
# be linked on a host). They pin the contract, not the behaviour: a refactor
# that drops one of these guards fails here on purpose.
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
src = (ROOT / "components/web_config/web_config.c").read_text()


def body(name):
    m = re.search(r"^static esp_err_t " + name + r"\(httpd_req_t \*req\)\n\{(.*?)^\}", src, re.M | re.S)
    assert m, name
    return m.group(1)


# POST routes that read a body reject unauthenticated calls with ESP_FAIL.
for h in ("playback_post", "podcasts_download_post", "debug_nav_post"):
    assert "REQUIRE_AUTH(req, ESP_FAIL)" in body(h), h

# memo_post: size check first, and its 413 closes the connection.
memo = body("memo_post")
size = memo.index("MEMO_RX_MAX_BYTES")
assert size < memo.index("ui.memo_rx") and size < memo.index("source_sd_present")
assert "return ESP_FAIL" in memo[size:memo.index("ui.memo_rx")]

# wifi_post validates lengths before writing any slot.
wifi = body("wifi_post")
assert wifi.index("CFG_WIFI_PASS_MAX)") < wifi.index("config_store_set_wifi_slot(slot++")
assert wifi.index("CFG_WIFI_SSID_MAX)") < wifi.index("config_store_set_wifi_slot(slot++")

# One OTA writer: both entry points take the same flag.
assert "atomic_flag_test_and_set(&s_ota_busy)" in body("ota_post")
gh = re.search(r"^esp_err_t web_config_gh_install\(void\)\n\{(.*?)^\}", src, re.M | re.S).group(1)
assert "atomic_flag_test_and_set(&s_ota_busy)" in gh

print("review web body/auth/OTA-lock rules: passed")
