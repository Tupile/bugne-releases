#!/usr/bin/env python3
"""Check that every config.json field config_store PARSES it also EMITS.

This guards one specific, already-experienced failure: device-side setters
(theme, alarm, favorites...) rewrite the whole file from the in-memory struct, so
a field that load_from_json reads but save_to_disk never writes is silently
dropped the next time the user taps a setting on the device. That is exactly what
happened to the `quiet` array before 2026-07-04.

A C round-trip test would need NVS, LittleFS and mbedTLS stubs for a file that is
mostly I/O; comparing the two field lists catches the same class for free. Run
from test/host/run.sh.
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "components/config_store/config_store.c"

# Parsed but deliberately not emitted.
PARSE_ONLY = {
    "alarm",  # legacy single-alarm object, read once then migrated to "alarms"
}
# Emitted but never read back (none today; kept so an addition is a decision).
EMIT_ONLY: set[str] = set()


def body_of(text: str, signature: str) -> str:
    """The braces-balanced body of the function whose definition starts with signature."""
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : i + 1]
    raise SystemExit(f"unbalanced braces after {signature!r}")


def main() -> int:
    text = SRC.read_text()
    # Parse side: file-wide, because the alarm object is parsed by its own helper
    # (parse_one_alarm) and schema_version is read by load_config.
    # Emit side: save_to_disk only, which is the single writer of the file.
    save = body_of(text, "static esp_err_t save_to_disk(")

    parsed = set(re.findall(r'cJSON_GetObjectItemCaseSensitive\([^,]+,\s*"([^"]+)"', text))
    emitted = set(re.findall(r'cJSON_Add\w+ToObject\([^,]+,\s*"([^"]+)"', save))

    missing = parsed - emitted - PARSE_ONLY
    extra = emitted - parsed - EMIT_ONLY

    for name in sorted(missing):
        print(f'FAIL: config field "{name}" is parsed but never written back: a '
              f"device-side setter would silently drop it")
    for name in sorted(extra):
        print(f'FAIL: config field "{name}" is written but never parsed back')

    if missing or extra:
        print(f"check_config_parity: {len(missing) + len(extra)} FAILURE(S)")
        return 1
    print(f"check_config_parity: {len(parsed)} parsed fields all round-trip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
