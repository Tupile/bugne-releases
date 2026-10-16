import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
src = (ROOT / "components/web_config/web_config.c").read_text()

def extract(name):
    # Match the function signature and its body
    match = re.search(r"^(static )?(int|void|bool) " + name + r"\([^)]*\)\n\{", src, re.M)
    assert match, f"Function {name} not found"

    start_idx = match.start()
    opening = src.index("{", match.end() - 1)
    depth = 1
    end = opening + 1
    while depth > 0 and end < len(src):
        if src[end] == "{":
            depth += 1
        elif src[end] == "}":
            depth -= 1
        end += 1

    return src[start_idx:end]

prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

// Mock esp timer
static int64_t s_mock_time = 0;
int64_t esp_timer_get_time(void) { return s_mock_time; }

// Mock portMUX
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static portMUX_TYPE s_gh_lock = portMUX_INITIALIZER_UNLOCKED;
void portENTER_CRITICAL(portMUX_TYPE *m) { (void)m; }
void portEXIT_CRITICAL(portMUX_TYPE *m) { (void)m; }

// Mock variables from web_config.c used by gh_cache functions
static char s_gh_latest[32];
static bool s_gh_update;
static bool s_gh_checked;
static int64_t s_gh_check_time_us;

// Provide strlcpy if not on macOS/BSD
static size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t srclen = strlen(src);
    if (size > 0) {
        size_t copylen = (srclen >= size) ? size - 1 : srclen;
        memcpy(dst, src, copylen);
        dst[copylen] = '\0';
    }
    return srclen;
}
'''

body = "\n\n".join(extract(name) for name in ("hexv", "url_decode", "gh_cache_store", "gh_cache_read", "web_config_gh_status"))

cases = r'''
static void test_hexv(void) {
    assert(hexv('0') == 0);
    assert(hexv('9') == 9);
    assert(hexv('a') == 10);
    assert(hexv('f') == 15);
    assert(hexv('A') == 10);
    assert(hexv('F') == 15);
    assert(hexv('G') == -1);
    assert(hexv('z') == -1);
    assert(hexv(' ') == -1);
}

static void test_url_decode(void) {
    char buf1[] = "hello%20world";
    url_decode(buf1);
    assert(strcmp(buf1, "hello world") == 0);

    char buf2[] = "%00hide"; // %00 keeps literal %00 instead of truncating
    url_decode(buf2);
    assert(strcmp(buf2, "%00hide") == 0);

    char buf3[] = "fus%C3%A9e"; // utf-8
    url_decode(buf3);
    assert(strcmp(buf3, "fus\xC3\xA9""e") == 0);

    char buf4[] = "test%ZZ"; // invalid hex
    url_decode(buf4);
    assert(strcmp(buf4, "test%ZZ") == 0); // unchanged

    char buf5[] = "%2B+"; // + decoded, + literal
    url_decode(buf5);
    assert(strcmp(buf5, "++") == 0);
}

static void test_gh_cache(void) {
    char latest[32] = {0};
    bool update = false;
    bool checked = false;
    long long age_s = -1;

    // Initial state
    assert(web_config_gh_status(latest, sizeof(latest), &update) == false);

    // Store
    s_mock_time = 1000000; // 1 second
    gh_cache_store("v1.2.3", true);

    // Read via status
    assert(web_config_gh_status(latest, sizeof(latest), &update) == true);
    assert(strcmp(latest, "v1.2.3") == 0);
    assert(update == true);

    // Check age logic inside read
    s_mock_time = 3000000; // 3 seconds, so age should be 2 seconds
    gh_cache_read(latest, sizeof(latest), &update, &checked, &age_s);
    assert(checked == true);
    assert(age_s == 2);
}

int main(void) {
    test_hexv();
    test_url_decode();
    test_gh_cache();
    printf("review web config logic: passed\n");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="bugne-web-config-") as directory:
    source = pathlib.Path(directory) / "test.c"
    binary = pathlib.Path(directory) / "test"
    source.write_text(prelude + body + cases)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
