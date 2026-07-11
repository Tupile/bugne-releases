#!/usr/bin/env python3
# Rebuild the slug -> livemeta id table for Radio France stations.
#
# Radio France's livemeta API (api.radiofrance.fr/livemeta/live/<id>/
# webrf_webradio_player) identifies stations by a numeric id, while their
# icecast streams are named by slug (icecast.radiofrance.fr/<slug>-<quality>.mp3).
# The firmware keeps a static slug -> id table in components/source_stream/
# rf_meta.c, between the marker lines "// BEGIN RF_STATIONS (generated)" and
# "// END RF_STATIONS (generated)". This script rebuilds that table content.
# It runs on the dev machine only, never on the device.
#
# POLITENESS RULE (do not remove): at most one request per second to any
# radiofrance.fr host, no parallelism. A prior recon scan that fired 20 requests
# in parallel got the dev IP temporarily banned by their Akamai front end. If a
# response looks like an Akamai "Access Denied" block page, this script stops
# and tells you to retry later.
#
# Phases:
#   1. id scan: probe a range of numeric ids against the livemeta API, print
#      the ones that respond with real data.
#   2. slug verification: check that each candidate slug's icecast stream
#      actually exists (HEAD-like Range request, no audio downloaded).
#   3. matching: pair up scanned ids and verified slugs by normalized name,
#      then print confirmed entries, the C table, and what is left unresolved.
#
# Usage:
#   python3 tools/rf_livemeta_map.py                    # full scan, ids 1..1000
#   python3 tools/rf_livemeta_map.py --ids 1-2000        # wider id range
#   python3 tools/rf_livemeta_map.py --skip-scan         # phase 2/3 only, fast
#   python3 tools/rf_livemeta_map.py --write             # also patch rf_meta.c
#
# Typical runtime: a full scan of 1000 ids plus slug verification takes about
# 20 minutes at 1 request/second. Use --skip-scan to iterate on phase 2/3 fast.

import argparse
import gzip
import json
import os
import sys
import time
import unicodedata
import urllib.error
import urllib.request

RATE_LIMIT_SLEEP = 1.0
API_HOST = "https://api.radiofrance.fr/livemeta"
ICECAST_HOST = "https://icecast.radiofrance.fr"
RF_META_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "..", "components", "source_stream", "rf_meta.c")
BEGIN_MARKER = "// BEGIN RF_STATIONS (generated)"
END_MARKER = "// END RF_STATIONS (generated)"

# (slug, human name hint, expected livemeta id or None if unknown)
CANDIDATES = [
    ("franceinter", "France Inter", 1),
    ("franceinfo", "France Info", 2),
    ("francemusique", "France Musique", 4),
    ("franceculture", "France Culture", 5),
    ("mouv", "Mouv", 6),
    ("fip", "Fip", 7),
    ("fiprock", "Fip Rock", 64),
    ("fipjazz", "Fip Jazz", 65),
    ("fipgroove", "Fip Groove", 66),
    ("fipworld", "Fip World", 69),
    ("fipnouveautes", "Fip Nouveautes", 70),
    ("fipreggae", "Fip Reggae", 71),
    ("fipelectro", "Fip Electro", 74),
    ("fipmetal", "Fip Metal", 77),
    ("fippop", "Fip Pop", 78),
    ("fiphiphop", "Fip Hip-Hop", 95),
    ("fipsacrefrancais", "Fip Sacre Francais", 96),
    ("francemusiqueeasyclassique", "France Musique Easy Classique", 401),
    ("francemusiqueclassiqueplus", "France Musique Classique Plus", 402),
    ("francemusiqueconcertsradiofrance", "France Musique Concerts Radio France", 403),
    ("francemusiqueocoramonde", "France Musique Ocora Monde", 404),
    ("francemusiquelajazz", "France Musique La Jazz", 405),
    ("francemusiquelacontemporaine", "France Musique La Contemporaine", 406),
    ("francemusiquelabo", "France Musique Labo", 407),
    ("francemusiquebaroque", "France Musique Baroque", 408),
    ("francemusiqueopera", "France Musique Opera", 409),
    ("fipcultes", "Fip Cultes", 709),
    ("franceinterlamusiqueinter", "France Inter La Musique d'Inter", 1101),
    ("monpetitfranceinter", "Mon Petit France Inter", 1102),
    ("montoutpetitfranceinter", "Mon Tout Petit France Inter", 1103),
    # mouvclassics, mouvdancehall, mouvrnb, mouvrapus, mouvrapfr, mouv100mix,
    # francemusiquefilms: dropped, streams gone or never existed (the film
    # music webradio slug is francemusiquelabo, La B.O.).
]

AKAMAI_MARKERS = (b"Access Denied", b"AkamaiGHost")


def check_akamai_ban(status, body):
    if status == 403 and body and any(m in body for m in AKAMAI_MARKERS):
        print("You are rate-banned by Akamai (HTTP 403, Access Denied). "
              "Stop making requests and retry in a few hours.", file=sys.stderr)
        sys.exit(1)


def http_get(url, headers, timeout=6):
    # One request, always preceded by the mandatory sleep. Returns
    # (status, body, content_encoding). status is None on network failure.
    time.sleep(RATE_LIMIT_SLEEP)
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            enc = resp.headers.get("Content-Encoding", "").lower()
            return resp.getcode(), body, enc
    except urllib.error.HTTPError as e:
        body = e.read()
        check_akamai_ban(e.code, body)
        return e.code, body, ""
    except Exception:
        return None, None, ""


def http_head_stream(url, headers, timeout=6):
    # Open a stream URL, read at most 1 byte, close immediately. Never
    # downloads audio. Returns the HTTP status, or None on failure.
    time.sleep(RATE_LIMIT_SLEEP)
    req = urllib.request.Request(url, headers=headers)
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
        status = resp.getcode()
        try:
            resp.read(1)
        finally:
            resp.close()
        return status
    except urllib.error.HTTPError as e:
        body = e.read(2048)
        check_akamai_ban(e.code, body)
        return e.code
    except Exception:
        return None


def parse_ids(spec):
    if "-" in spec:
        a, b = spec.split("-", 1)
        return range(int(a), int(b) + 1)
    n = int(spec)
    return range(n, n + 1)


def scan_ids(id_range):
    # Phase 1. Returns {id: name}.
    found = {}
    for station_id in id_range:
        if station_id % 100 == 0:
            print(f"... scanning, at id={station_id}", file=sys.stderr)

        # Identity is required, not just a fallback fetch: this endpoint gzips
        # its response otherwise. Kept as a gzip-decode fallback just in case.
        status, body, enc = http_get(
            f"{API_HOST}/live/{station_id}/webrf_webradio_player",
            {"Accept-Encoding": "identity"})
        if status != 200 or not body:
            continue
        try:
            if enc == "gzip" or body[:2] == b"\x1f\x8b":
                body = gzip.decompress(body)
            data = json.loads(body)
        except Exception:
            continue
        now = data.get("now")
        if not isinstance(now, dict) or not now.get("firstLine"):
            continue

        prev = data.get("prev") or []
        name = None
        if prev and isinstance(prev[0], dict):
            name = prev[0].get("secondLine")
        name = name or now.get("secondLine") or now.get("firstLine")

        found[station_id] = name
        print(f"id={station_id} name='{name}'")
    return found


def verify_slug(slug):
    # Phase 2, one candidate. Returns True if the icecast stream exists.
    for suffix in ("-midfi.mp3", "-hifi.aac"):
        status = http_head_stream(f"{ICECAST_HOST}/{slug}{suffix}",
                                   {"Range": "bytes=0-0"})
        if status in (200, 206):
            return True
    return False


def normalize(name):
    # lowercase, strip accents, drop spaces/punctuation, drop noise words.
    s = unicodedata.normalize("NFD", name)
    s = "".join(c for c in s if not unicodedata.combining(c))
    s = s.lower()
    s = "".join(c for c in s if c.isalnum())
    for noise in ("ledirect", "de", "la", "le"):
        s = s.replace(noise, "")
    return s


def guess_slug_for_name(slug, norm_name):
    # Simple, deliberately loose heuristic: match a prefixed slug family
    # (fip/mouv/francemusique) against the rest of the normalized name, or
    # fall back to plain substring containment.
    for prefix in ("fip", "mouv", "francemusique"):
        if slug.startswith(prefix):
            remainder = slug[len(prefix):]
            if remainder and remainder in norm_name:
                return True
    return slug in norm_name or norm_name in slug


def write_table(confirmed, path=RF_META_PATH):
    try:
        with open(path) as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"error: {path} not found, cannot --write", file=sys.stderr)
        sys.exit(1)

    begin_idx = end_idx = None
    for i, line in enumerate(lines):
        if BEGIN_MARKER in line:
            begin_idx = i
        elif END_MARKER in line and begin_idx is not None:
            end_idx = i
            break
    if begin_idx is None or end_idx is None:
        print(f"error: markers not found in {path}", file=sys.stderr)
        sys.exit(1)

    table_lines = [f'    {{"{slug}", {sid}}},\n'
                   for slug, sid in sorted(confirmed.items(), key=lambda kv: kv[1])]
    new_lines = lines[:begin_idx + 1] + table_lines + lines[end_idx:]
    with open(path, "w") as f:
        f.writelines(new_lines)
    print(f"wrote {len(confirmed)} entries to {path}")


def main():
    parser = argparse.ArgumentParser(
        description="Rebuild the Radio France slug -> livemeta id table.")
    parser.add_argument("--ids", default="1-1000",
                         help="id range to scan in phase 1, e.g. 1-2000 (default 1-1000)")
    parser.add_argument("--skip-scan", action="store_true",
                         help="skip phase 1, use only the embedded verified pairs")
    parser.add_argument("--write", action="store_true",
                         help="patch rf_meta.c with the confirmed entries")
    args = parser.parse_args()

    scanned = {} if args.skip_scan else scan_ids(parse_ids(args.ids))

    stream_ok = {}
    for slug, name_hint, expected_id in CANDIDATES:
        ok = verify_slug(slug)
        stream_ok[slug] = ok
        status = "OK" if ok else "NOT FOUND"
        print(f"slug={slug} stream={status}", file=sys.stderr)

    confirmed = {slug: expected_id for slug, name_hint, expected_id in CANDIDATES
                 if expected_id is not None and stream_ok.get(slug)}

    dropped = [slug for slug, name_hint, expected_id in CANDIDATES
               if expected_id is not None and not stream_ok.get(slug)]

    matched_ids = set(confirmed.values())
    unresolved_slugs = [slug for slug, name_hint, expected_id in CANDIDATES
                         if expected_id is None and stream_ok.get(slug)]
    unresolved_ids = {sid: name for sid, name in scanned.items() if sid not in matched_ids}

    print("\n== confirmed ==")
    for slug, sid in sorted(confirmed.items(), key=lambda kv: kv[1]):
        print(f"{slug}={sid}")

    print("\n== C table ==")
    for slug, sid in sorted(confirmed.items(), key=lambda kv: kv[1]):
        print(f'    {{"{slug}", {sid}}},')

    print("\n== unresolved ==")
    if dropped:
        for slug in dropped:
            print(f"WARNING: seed pair '{slug}' failed stream verification, dropped from confirmed")
    for slug in unresolved_slugs:
        guesses = [f"id={sid} name='{name}'" for sid, name in unresolved_ids.items()
                   if guess_slug_for_name(slug, normalize(name))]
        if guesses:
            print(f"slug={slug} stream=OK, no id assigned. LOW-CONFIDENCE match: {', '.join(guesses)}")
        else:
            print(f"slug={slug} stream=OK, no id assigned, no name match found")
    for sid, name in sorted(unresolved_ids.items()):
        guesses = [slug for slug in unresolved_slugs
                   if guess_slug_for_name(slug, normalize(name))]
        if not guesses:
            print(f"id={sid} name='{name}', no slug found")

    if args.write:
        write_table(confirmed)


if __name__ == "__main__":
    main()
