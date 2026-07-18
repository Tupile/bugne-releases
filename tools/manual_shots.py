#!/usr/bin/env python3
# Capture the user-manual screenshots from a Bugne device, one pass per
# language. For each language: set ui.lang via GET+modify+POST /api/config
# (full-replace endpoint), navigate every screen in SCREENS with
# POST /api/debug/nav, save GET /api/screenshot as PNG. now_playing is
# captured while a web radio plays (started via POST /api/playback).
# The original ui.lang is restored at the end, even on error.
#
# Usage: python3 tools/manual_shots.py <ip> [--login <password>]
#        [--langs en,fr] [--outdir docs/manual/img] [--only <screen,...>]
#
# Reuses the BMP-to-PNG conversion from tools/screenshot.py (stdlib only).
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screenshot import bmp565_to_png

# (nav name, output file base). Order matters: static screens first,
# now_playing last (playback would add the mini bar to every later shot).
SCREENS = [
    ("home", "home"),
    ("webradios", "webradios"),
    ("podcasts", "podcasts"),
    ("episodes", "episodes"),
    ("sd", "sd"),
    ("library", "library"),
    ("favorites", "favorites"),
    ("game", "game_setup"),
    ("game_play", "game_play"),
    ("tuner", "tuner"),
    # For a non-empty memos list, seed one received memo first:
    # curl -X POST --data-binary @<16kHz-mono-pcm16.wav> "http://<ip>/api/memo?from=Papa"
    # and delete it afterwards (POST /api/sd/delete?path=memos).
    ("memos", "memos"),
    ("memo_record", "memo_record"),
    ("settings", "settings"),
    ("settings_theme", "settings_theme"),
    ("settings_alarm", "settings_alarm"),
    # settings_web, settings_ap and setup are deliberately NOT captured: those
    # screens show the real device hostname/IP and QR codes that embed the
    # setup-AP password, so their shots must never land in the public manual.
    ("now_playing", "now_playing"),  # special-cased: radio playing
]

LANG_APPLY_S = 3.0   # sleep_timer_cb live-apply after a config change
NAV_SETTLE_S = 1.5   # screen build + LVGL refresh
META_WAIT_S = 6.0    # radio connect + first metadata


class Device:
    def __init__(self, ip, password=None):
        self.ip = ip
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor())
        if password:
            self.post("/login", {"pass": password})

    def get(self, path):
        return self.opener.open("http://%s%s" % (self.ip, path), timeout=15)

    def post(self, path, obj):
        req = urllib.request.Request(
            "http://%s%s" % (self.ip, path), data=json.dumps(obj).encode(),
            headers={"Content-Type": "application/json"})
        return self.opener.open(req, timeout=15).read()

    def config(self):
        return json.load(self.get("/api/config"))

    def set_ui(self, cfg, lang, orientation):
        # POST the FULL config back: /api/config is a full-replace endpoint,
        # a partial body would wipe every other setting.
        cfg.setdefault("ui", {})["lang"] = lang
        cfg["ui"]["orientation"] = orientation
        self.post("/api/config", cfg)
        time.sleep(LANG_APPLY_S)

    def nav(self, screen):
        self.post("/api/debug/nav", {"screen": screen})
        time.sleep(NAV_SETTLE_S)

    def screenshot(self, out_path):
        resp = self.get("/api/screenshot")
        data = resp.read()
        ctype = resp.headers.get("Content-Type", "").split(";")[0]
        if ctype != "image/bmp":
            raise SystemExit("screenshot: not a BMP (%r)" % data[:120])
        with open(out_path, "wb") as f:
            f.write(bmp565_to_png(data))
        print("wrote %s" % out_path)


def capture_pass(dev, lang, outdir, only):
    d = os.path.join(outdir, lang)
    os.makedirs(d, exist_ok=True)
    for nav_name, base in SCREENS:
        if only and base not in only and nav_name not in only:
            continue
        playing = nav_name == "now_playing"
        if playing:
            dev.post("/api/playback", {"action": "radio", "value": 0})
            time.sleep(META_WAIT_S)
        try:
            dev.nav(nav_name)
            dev.screenshot(os.path.join(d, base + ".png"))
        finally:
            if playing:
                dev.post("/api/playback", {"action": "stop"})


def main():
    args = sys.argv[1:]
    password, langs, outdir, only = None, ["en", "fr"], "docs/manual/img", None
    for flag in ("--login", "--langs", "--outdir", "--only"):
        if flag in args:
            i = args.index(flag)
            val = args[i + 1]
            del args[i:i + 2]
            if flag == "--login":
                password = val
            elif flag == "--langs":
                langs = val.split(",")
            elif flag == "--outdir":
                outdir = val
            else:
                only = set(val.split(","))
    if len(args) != 1:
        raise SystemExit("usage: manual_shots.py <ip> [--login <password>] "
                         "[--langs en,fr] [--outdir dir] [--only screen,...]")

    dev = Device(args[0], password)
    cfg = dev.config()
    orig_lang = cfg.get("ui", {}).get("lang", "en")
    orig_orient = cfg.get("ui", {}).get("orientation", 0)
    try:
        for lang in langs:
            print("== language: %s ==" % lang)
            dev.set_ui(cfg, lang, 0)  # manual shots are portrait
            capture_pass(dev, lang, outdir, only)
    finally:
        dev.post("/api/playback", {"action": "stop"})
        dev.set_ui(cfg, orig_lang, orig_orient)
        dev.nav("home")
        print("restored ui.lang=%s orientation=%d" % (orig_lang, orig_orient))


if __name__ == "__main__":
    main()
