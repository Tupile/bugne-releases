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


def ensure_wokwi_cli():
    """Ensure wokwi-cli is installed, or attempt automatic installation."""
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
        cmd = f". {export_script} && idf.py build"
    else:
        cmd = "idf.py build"

    res = subprocess.run(cmd, shell=True, cwd=PROJECT_DIR)
    if res.returncode != 0:
        print("[Build] [FAIL] Build failed with exit code", res.returncode)
        sys.exit(res.returncode)
    print("[Build] [OK] Firmware build successful.")


def run_scenario(wokwi_bin, scenario_path, screenshot_dir, timeout_ms=20000):
    """Run a single Wokwi scenario test headlessly."""
    scenario_name = os.path.splitext(os.path.basename(scenario_path))[0]
    os.makedirs(screenshot_dir, exist_ok=True)
    screenshot_file = os.path.join(screenshot_dir, f"{scenario_name}.png")

    print(f"\n{'='*70}")
    print(f"Running Scenario: {scenario_name}")
    print(f"Scenario File:   {scenario_path}")
    print(f"Screenshot Target: {screenshot_file}")
    print(f"{'='*70}")

    # Build command
    cmd = [
        wokwi_bin,
        PROJECT_DIR,
        "--scenario", scenario_path,
        "--timeout", str(timeout_ms),
        "--fail-text", "Guru Meditation Error",
        "--fail-text", "abort()",
        "--fail-text", "CORRUPT HEAP",
        "--fail-text", "Backtrace:",
        "--fail-text", "panic_abort",
        "--screenshot-part", "lcd",
        "--screenshot-time", str(max(1000, timeout_ms - 2000)),
        "--screenshot-file", screenshot_file,
    ]

    env = os.environ.copy()
    if "WOKWI_CLI_TOKEN" not in env:
        # Check standard config or warn
        print("[Wokwi] Note: WOKWI_CLI_TOKEN is not explicitly exported in the current shell.")

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
        for line in iter(proc.stdout.readline, ''):
            sys.stdout.write(line)
            logs.append(line)
        proc.stdout.close()
        return_code = proc.wait()
        elapsed = time.time() - start_time

        if return_code == 0:
            print(f"\n[PASS] Scenario '{scenario_name}' completed successfully in {elapsed:.1f}s")
            if os.path.isfile(screenshot_file):
                print(f"[Screenshot] Saved at: {screenshot_file}")
            return True
        else:
            print(f"\n[FAIL] Scenario '{scenario_name}' failed with exit code {return_code} after {elapsed:.1f}s")
            return False

    except KeyboardInterrupt:
        print("\n[Wokwi] Test interrupted by user.")
        return False
    except Exception as e:
        print(f"\n[FAIL] Error executing Wokwi CLI: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description="Bugne Wokwi Headless Simulation Runner")
    parser.add_argument("--build", action="store_true", help="Build firmware before running simulation")
    parser.add_argument("--scenario", type=str, help="Specific scenario file to run")
    parser.add_argument("--all", action="store_true", help="Run all scenario tests sequentially")
    parser.add_argument("--screenshot-dir", type=str, default=DEFAULT_SCREENSHOT_DIR, help="Directory for screenshots")
    parser.add_argument("--timeout", type=int, default=20000, help="Simulation timeout per test in milliseconds")
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
