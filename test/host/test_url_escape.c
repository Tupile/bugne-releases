// Host tests for components/http_util/url_escape.c.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "url_escape.h"

static void check(const char *in, const char *want)
{
    char out[512];
    assert(url_escape(in, out, sizeof(out)));
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "url_escape(%s)\n  got  %s\n  want %s\n", in, out, want);
        assert(0);
    }
    assert(url_escape_len(in) == strlen(want));
    char *dup = url_escape_dup(in);
    assert(dup && strcmp(dup, want) == 0);
    free(dup);
}

int main(void)
{
    // Plain ASCII URLs, query strings and existing escapes are untouched.
    check("https://audio.ausha.co/bYV9LhxZXDR5.mp3?t=1790326656&x=a%20b",
          "https://audio.ausha.co/bYV9LhxZXDR5.mp3?t=1790326656&x=a%20b");
    check("http://host:8080/a/b;c=d#frag", "http://host:8080/a/b;c=d#frag");

    // Real CNES enclosure: 'e' + U+0301 (NFD), kept byte for byte. The server
    // answers 206 for exactly this form and 404 for the NFC %C3%A9 form.
    check("https://podcast.cnes.fr/wp-content/uploads/2022/07/"
          "2022_07_04_quelle-est-la-premiere-fuse\xcc\x81" "e-qui-a-existe-au-monde_MIXE.mp3",
          "https://podcast.cnes.fr/wp-content/uploads/2022/07/"
          "2022_07_04_quelle-est-la-premiere-fuse%CC%81e-qui-a-existe-au-monde_MIXE.mp3");
    // NFC input stays NFC.
    check("https://x/fus\xc3\xa9" "e.mp3", "https://x/fus%C3%A9e.mp3");

    // Space, quote, angle brackets, backslash, braces, pipe, caret, backtick.
    check("https://x/a b\"<>\\^`{|}", "https://x/a%20b%22%3C%3E%5C%5E%60%7B%7C%7D");
    // Controls and DEL.
    check("a\tb\x7f", "a%09b%7F");
    check("", "");

    // Too small a buffer fails cleanly with an empty string.
    char small[8];
    assert(!url_escape("https://x/\xc3\xa9", small, sizeof(small)));
    assert(small[0] == '\0');
    char exact[sizeof("a%20b")];
    assert(url_escape("a b", exact, sizeof(exact)) && strcmp(exact, "a%20b") == 0);
    assert(!url_escape(NULL, exact, sizeof(exact)));
    assert(url_escape_dup(NULL) == NULL);

    puts("url_escape: ascii passthrough, NFD/NFC bytes kept, illegal chars, bounds passed");
    return 0;
}
