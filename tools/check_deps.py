#!/usr/bin/env python3
"""Bugne dependency and library checker.

Audits external dependencies against the ESP Component Registry and GitHub.
Usage:
    python3 tools/check_deps.py [--json] [--verbose]
"""

import json
import os
import re
import sys
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MANIFEST_PATH = os.path.join(ROOT_DIR, "main", "idf_component.yml")
LOCK_PATH = os.path.join(ROOT_DIR, "dependencies.lock")

REGISTRY_API_BASE = "https://components.espressif.com/api/components"
USER_AGENT = "Bugne-Dep-Checker/1.0"


def parse_manifest(path: str) -> Dict[str, str]:
    """Extract declared dependencies from idf_component.yml without requiring external yaml lib."""
    deps: Dict[str, str] = {}
    if not os.path.exists(path):
        return deps

    current_dep: Optional[str] = None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if stripped == "dependencies:":
                continue

            # Match component name "namespace/name:" or "idf:"
            match_name = re.match(r"^([a-zA-Z0-9_\-]+/[a-zA-Z0-9_\-]+|idf):\s*$", stripped)
            if match_name:
                current_dep = match_name.group(1)
                continue

            if current_dep and stripped.startswith("version:"):
                v_match = re.search(r'version:\s*["\']?([^"\']+)["\']?', stripped)
                if v_match:
                    deps[current_dep] = v_match.group(1).strip()
                current_dep = None
    return deps


def parse_lock(path: str) -> Dict[str, str]:
    """Extract locked versions from dependencies.lock."""
    locked: Dict[str, str] = {}
    if not os.path.exists(path):
        return locked

    current_dep: Optional[str] = None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if stripped in ("dependencies:", "direct_dependencies:"):
                current_dep = None
                continue

            match_name = re.match(r"^([a-zA-Z0-9_\-]+/[a-zA-Z0-9_\-]+|idf):\s*$", stripped)
            if match_name:
                current_dep = match_name.group(1)
                continue

            if current_dep and stripped.startswith("version:"):
                v_match = re.search(r'version:\s*["\']?([^"\']+)["\']?', stripped)
                if v_match:
                    locked[current_dep] = v_match.group(1).strip()
                    current_dep = None
    return locked


def fetch_registry_versions(component: str) -> Tuple[Optional[str], List[str], Optional[str]]:
    """Fetch recent versions and latest stable version for a component from Espressif registry."""
    if component == "idf" or "/" not in component:
        return None, [], None

    ns, name = component.split("/", 1)
    url = f"{REGISTRY_API_BASE}/{ns}/{name}"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})

    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            versions_list = [v["version"] for v in data.get("versions", [])]
            if not versions_list:
                return None, [], None

            latest_all = versions_list[0]
            # Find latest stable (excluding beta, alpha, rc, pre)
            latest_stable = None
            for v in versions_list:
                if not re.search(r"(alpha|beta|rc|dev|preview)", v, re.IGNORECASE):
                    latest_stable = v
                    break

            return latest_stable, versions_list[:5], latest_all
    except Exception as e:
        return None, [], f"Error: {e}"


def check_vendored_libs() -> List[Dict[str, Any]]:
    """Inspect local vendored single-file libraries."""
    libs = [
        {
            "name": "dr_mp3 (minimp3 fork)",
            "path": "components/decode/dr_mp3.h",
            "regex": r"dr_mp3\s*-\s*v?([0-9\.]+)",
            "upstream_repo": "mackron/dr_libs",
            "upstream_file": "dr_mp3.h",
        },
        {
            "name": "dr_flac",
            "path": "components/decode/dr_flac.h",
            "regex": r"dr_flac\s*-\s*v?([0-9\.]+)",
            "upstream_repo": "mackron/dr_libs",
            "upstream_file": "dr_flac.h",
        },
        {
            "name": "minimp4",
            "path": "components/decode/minimp4.h",
            "regex": r"https://github\.com/(lieff|aspt)/minimp4",
            "upstream_repo": "lieff/minimp4",
            "upstream_file": "minimp4.h",
        },
        {
            "name": "yxml",
            "path": "components/podcast/yxml.h",
            "regex": r"Copyright\s*\(c\)\s*2013-2014\s*Yoran Heling",
            "upstream_repo": None,
            "upstream_file": None,
        },
    ]

    results = []
    for lib in libs:
        full_path = os.path.join(ROOT_DIR, lib["path"])
        local_ver = "unknown"
        if os.path.exists(full_path):
            with open(full_path, "r", encoding="utf-8", errors="replace") as f:
                content = f.read(2048)
                m = re.search(lib["regex"], content)
                if m:
                    local_ver = m.group(1) if m.lastindex else "detected"

        results.append({
            "name": lib["name"],
            "path": lib["path"],
            "local_version": local_ver,
            "upstream_repo": lib["upstream_repo"],
        })
    return results


def main() -> int:
    verbose = "--verbose" in sys.argv
    as_json = "--json" in sys.argv

    manifest_deps = parse_manifest(MANIFEST_PATH)
    lock_deps = parse_lock(LOCK_PATH)

    # Union of direct deps + notable transitive deps
    all_components = list(manifest_deps.keys())
    for extra in ["bblanchon/arduinojson", "esphome/micro-flac", "esphome/micro-opus", "espressif/esp_lcd_touch"]:
        if extra not in all_components:
            all_components.append(extra)

    report: List[Dict[str, Any]] = []

    if not as_json:
        print("=" * 88)
        print(f"{'Component':<32} {'Manifest':<12} {'Locked':<10} {'Latest Stable':<14} {'Status':<16}")
        print("=" * 88)

    for cmp in all_components:
        if cmp == "idf":
            continue
        manifest_ver = manifest_deps.get(cmp, "(transitive)")
        locked_ver = lock_deps.get(cmp, "-")
        latest_stable, recent, latest_all = fetch_registry_versions(cmp)

        status = "OK"
        if not latest_stable:
            status = "UNKNOWN"
        elif locked_ver != "-" and locked_ver != latest_stable:
            status = f"UPDATE -> {latest_stable}"
        elif latest_all and latest_all != latest_stable:
            status = f"OK (Beta {latest_all})"

        entry = {
            "component": cmp,
            "manifest": manifest_ver,
            "locked": locked_ver,
            "latest_stable": latest_stable,
            "latest_all": latest_all,
            "recent_versions": recent,
            "status": status,
        }
        report.append(entry)

        if not as_json:
            print(f"{cmp:<32} {manifest_ver:<12} {locked_ver:<10} {str(latest_stable):<14} {status:<16}")
            if verbose and recent:
                print(f"   Recent: {', '.join(recent)}")

    vendored = check_vendored_libs()

    if as_json:
        output = {
            "managed_components": report,
            "vendored_libraries": vendored,
        }
        print(json.dumps(output, indent=2))
        return 0

    print("\n" + "=" * 88)
    print(f"{'Vendored Single-File Lib':<32} {'Local Version / Marker':<30} {'Status'}")
    print("=" * 88)
    for v in vendored:
        print(f"{v['name']:<32} {v['local_version']:<30} OK (Vendored)")

    print("=" * 88)
    return 0


if __name__ == "__main__":
    sys.exit(main())
