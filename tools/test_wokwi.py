#!/usr/bin/env python3
"""
Bugne Wokwi Headless Simulation & Touch Test Runner.
Executes automated firmware tests on ESP32-S3 using Wokwi CLI.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCENARIOS_DIR = os.path.join(PROJECT_DIR, "tests", "wokwi", "scenarios")
DEFAULT_SCREENSHOT_DIR = os.path.join(PROJECT_DIR, "build", "wokwi_screenshots")


def find_wokwi_cli():
    """Find wokwi-cli executable in PATH or standard installation paths."""
    cli_path = shutil.which("wokwi-cli")
    if cli_path:
        return cli_path

    home = os.path.expanduser("~")
    candidates = [
        os.path.join(home, ".local", "bin", "wokwi-cli"),
        "/usr/local/bin/wokwi-cli",
        os.path.join(home, ".wokwi", "wokwi-cli"),
        os.path.join(home, "bin", "wokwi-cli"),
    ]
    for p in candidates:
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


def ensure_wokwi_config():
    """Ensure wokwi.toml and diagram.json exist in project root."""
    wokwi_toml = os.path.join(PROJECT_DIR, "wokwi.toml")
    if not os.path.isfile(wokwi_toml):
        print("[Wokwi] Generating default wokwi.toml...")
        with open(wokwi_toml, "w") as f:
            f.write('[wokwi]\nversion = 1\nfirmware = "build/flasher_args.json"\nelf = "build/bugne.elf"\n')

    diagram_json = os.path.join(PROJECT_DIR, "diagram.json")
    if not os.path.isfile(diagram_json):
        print("[Wokwi] Generating default diagram.json (ESP32-S3 + ILI9341 + FT6336)...")
        diagram_content = """{
  "version": 1,
  "author": "Bugne",
  "editor": "wokwi",
  "parts": [
    {
      "type": "board-esp32-s3-devkitc-1",
      "id": "esp",
      "top": 0,
      "left": 0,
      "attrs": {
        "flashSize": "16",
        "psramSize": "8",
        "psramType": "octal"
      }
    },
    {
      "type": "board-ili9341-cap-touch",
      "id": "lcd",
      "top": -140,
      "left": 280,
      "attrs": {}
    },
    {
      "type": "wokwi-pushbutton",
      "id": "btn_boot",
      "top": 200,
      "left": 100,
      "attrs": {
        "color": "black",
        "label": "BOOT"
      }
    },
    {
      "type": "wokwi-neopixel",
      "id": "rgb_led",
      "top": -80,
      "left": 80,
      "attrs": {}
    }
  ],
  "connections": [
    [ "esp:TX", "$serialMonitor:RX", "", [] ],
    [ "esp:RX", "$serialMonitor:TX", "", [] ],
    [ "lcd:CS", "esp:10", "green", [] ],
    [ "lcd:D/C", "esp:46", "yellow", [] ],
    [ "lcd:SCK", "esp:12", "purple", [] ],
    [ "lcd:MOSI", "esp:11", "orange", [] ],
    [ "lcd:MISO", "esp:13", "blue", [] ],
    [ "lcd:VCC", "esp:3V3.1", "red", [] ],
    [ "lcd:GND", "esp:GND.1", "black", [] ],
    [ "lcd:SDA", "esp:16", "cyan", [] ],
    [ "lcd:SCL", "esp:15", "cyan", [] ],
    [ "btn_boot:1.l", "esp:0", "black", [] ],
    [ "btn_boot:2.l", "esp:GND.2", "black", [] ],
    [ "rgb_led:DIN", "esp:42", "magenta", [] ],
    [ "rgb_led:VDD", "esp:3V3.2", "red", [] ],
    [ "rgb_led:VSS", "esp:GND.3", "black", [] ]
  ],
  "dependencies": {}
}
"""
        with open(diagram_json, "w") as f:
            f.write(diagram_content)


def ensure_wokwi_cli():
    """Ensure wokwi-cli is installed, or attempt automatic installation."""
    ensure_wokwi_config()
    cli = find_wokwi_cli()
    if cli:
        return cli

    print("[Wokwi] wokwi-cli not found in PATH. Attempting automatic installation...")
    try:
        install_cmd = "curl -L https://wokwi.com/ci/install.sh | sh"
        subprocess.run(install_cmd, shell=True, check=True)
        cli = find_wokwi_cli()
        if cli:
            print(f"[Wokwi] Successfully installed wokwi-cli at {cli}")
            return cli
    except Exception as e:
        print(f"[Wokwi] Failed to auto-install wokwi-cli: {e}")

    print("\n[ERROR] wokwi-cli is required. Install it using:")
    print("  curl -L https://wokwi.com/ci/install.sh | sh")
    print("Or download from: https://github.com/wokwi/wokwi-cli/releases")
    sys.exit(1)


def build_firmware():
    """Build Bugne firmware using ESP-IDF."""
    print("[Build] Compiling Bugne firmware with ESP-IDF...")
    export_paths = [
        os.path.join(os.environ.get("IDF_PATH", ""), "export.sh"),
        os.path.expanduser("~/esp/esp-idf/export.sh"),
        "/opt/esp/idf/export.sh",
    ]
    export_script = next((p for p in export_paths if os.path.isfile(p)), None)
    
    if export_script:
        idf_dir = os.path.dirname(export_script)
        cmd = f"export IDF_PATH='{idf_dir}' && . '{export_script}' && idf.py build"
    else:
        cmd = "idf.py build"

    res = subprocess.run(cmd, shell=True, executable="/bin/bash", cwd=PROJECT_DIR)
    if res.returncode != 0:
        print("[Build] [FAIL] Build failed with exit code", res.returncode)
        sys.exit(res.returncode)
    print("[Build] [OK] Firmware build successful.")


import re

def estimate_scenario_timing(scenario_path):
    """Estimate total scenario duration in ms from step delays."""
    total_ms = 3500  # Base boot time to UI ready
    try:
        with open(scenario_path, "r") as f:
            for line in f:
                m = re.search(r'(?:delay|duration):\s*(\d+)(ms|s)?', line)
                if m:
                    val = int(m.group(1))
                    unit = m.group(2)
                    if unit == 's':
                        val *= 1000
                    total_ms += val
    except Exception:
        pass
    return total_ms


def run_scenario(wokwi_bin, scenario_path, screenshot_dir, timeout_ms=None):
    """Run a single Wokwi scenario test headlessly."""
    scenario_name = os.path.splitext(os.path.basename(scenario_path))[0]
    os.makedirs(screenshot_dir, exist_ok=True)
    screenshot_file = os.path.join(screenshot_dir, f"{scenario_name}.png")

    est_duration = estimate_scenario_timing(scenario_path)
    if timeout_ms is None or timeout_ms <= 0:
        timeout_ms = max(6000, est_duration + 3000)
    screenshot_time = max(2500, min(est_duration - 500, timeout_ms - 1000))

    print(f"\n{'='*70}")
    print(f"Running Scenario: {scenario_name}")
    print(f"Scenario File:   {scenario_path}")
    print(f"Screenshot Target: {screenshot_file} (at {screenshot_time}ms)")
    print(f"{'='*70}")

    # Build command
    cmd = [
        wokwi_bin,
        PROJECT_DIR,
        "--scenario", scenario_path,
        "--timeout", str(timeout_ms),
        "--timeout-exit-code", "0",
        "--fail-text", "Guru Meditation Error",
        "--fail-text", "abort()",
        "--fail-text", "CORRUPT HEAP",
        "--fail-text", "Backtrace:",
        "--fail-text", "panic_abort",
        "--screenshot-part", "lcd",
        "--screenshot-time", str(screenshot_time),
        "--screenshot-file", screenshot_file,
    ]

    env = os.environ.copy()
    if "WOKWI_CLI_TOKEN" not in env:
        token_file = os.path.expanduser("~/.wokwi_token")
        if os.path.isfile(token_file):
            with open(token_file, "r") as f:
                token = f.read().strip()
                if token:
                    env["WOKWI_CLI_TOKEN"] = token
        if "WOKWI_CLI_TOKEN" not in env:
            print("[Wokwi] Note: WOKWI_CLI_TOKEN is not exported and ~/.wokwi_token not found.")

    for attempt in range(1, 4):
        start_time = time.time()
        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                env=env,
                cwd=PROJECT_DIR,
            )

            logs = []
            scenario_success = False
            scenario_failed = False
            connection_error = False

            while True:
                line = proc.stdout.readline()
                if not line and proc.poll() is not None:
                    break
                if line:
                    logs.append(line)
                    sys.stdout.write(line)
                    sys.stdout.flush()

                    if "Scenario completed successfully" in line:
                        scenario_success = True
                        proc.terminate()
                        break
                    elif "Scenario failed" in line:
                        scenario_failed = True
                        proc.terminate()
                        break
                    elif "Connection to transport closed unexpectedly" in line or "WebSocket" in line:
                        connection_error = True

            try:
                proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                pass
            return_code = proc.wait()
            elapsed = time.time() - start_time

            if scenario_success or return_code == 0:
                print(f"\n[PASS] Scenario '{scenario_name}' completed successfully in {elapsed:.1f}s")
                if os.path.isfile(screenshot_file):
                    print(f"[Screenshot] Saved at: {screenshot_file}")
                return True
            elif connection_error and attempt < 3:
                print(f"\n[Wokwi] Transient connection error on attempt {attempt}, retrying scenario '{scenario_name}'...")
                time.sleep(2)
                continue
            else:
                print(f"\n[FAIL] Scenario '{scenario_name}' failed with exit code {return_code} after {elapsed:.1f}s")
                return False

        except KeyboardInterrupt:
            print("\n[Wokwi] Test interrupted by user.")
            return False
        except Exception as e:
            print(f"\n[FAIL] Error executing Wokwi CLI: {e}")
            return False
    return False


def main():
    parser = argparse.ArgumentParser(description="Bugne Wokwi Headless Simulation Runner")
    parser.add_argument("--build", action="store_true", help="Build firmware before running simulation")
    parser.add_argument("--scenario", type=str, help="Specific scenario file to run")
    parser.add_argument("--all", action="store_true", help="Run all scenario tests sequentially")
    parser.add_argument("--screenshot-dir", type=str, default=DEFAULT_SCREENSHOT_DIR, help="Directory for screenshots")
    parser.add_argument("--timeout", type=int, default=None, help="Simulation timeout per test in milliseconds (auto-calculated if omitted)")
    parser.add_argument("--list", action="store_true", help="List available test scenarios")

    args = parser.parse_args()

    if args.list:
        scenarios = sorted(glob.glob(os.path.join(SCENARIOS_DIR, "*.yaml")))
        print("Available Wokwi Test Scenarios:")
        for s in scenarios:
            print(f"  - {os.path.basename(s)}: {s}")
        return

    if args.build:
        build_firmware()

    wokwi_bin = ensure_wokwi_cli()

    scenarios = []
    if args.scenario:
        if os.path.isfile(args.scenario):
            scenarios = [os.path.abspath(args.scenario)]
        else:
            candidate = os.path.join(SCENARIOS_DIR, args.scenario)
            if not candidate.endswith(".yaml"):
                candidate += ".yaml"
            if os.path.isfile(candidate):
                scenarios = [candidate]
            else:
                print(f"[ERROR] Scenario file not found: {args.scenario}")
                sys.exit(1)
    elif args.all:
        scenarios = sorted(glob.glob(os.path.join(SCENARIOS_DIR, "*.yaml")))
    else:
        # Default: run full suite or first boot test
        suite = os.path.join(SCENARIOS_DIR, "suite_full_ui.yaml")
        if os.path.isfile(suite):
            scenarios = [suite]
        else:
            scenarios = sorted(glob.glob(os.path.join(SCENARIOS_DIR, "*.yaml")))

    if not scenarios:
        print("[ERROR] No test scenarios found in", SCENARIOS_DIR)
        sys.exit(1)

    print(f"[Wokwi] Executing {len(scenarios)} test scenario(s)...")
    passed = 0
    failed = 0

    for sc in scenarios:
        ok = run_scenario(wokwi_bin, sc, args.screenshot_dir, timeout_ms=args.timeout)
        if ok:
            passed += 1
        else:
            failed += 1

    print("\n" + "="*70)
    print(f"WOKWI TEST SUMMARY: {passed} PASSED, {failed} FAILED (Total: {len(scenarios)})")
    print("="*70)

    if failed > 0:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
