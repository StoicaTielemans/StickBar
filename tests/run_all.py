#!/usr/bin/env python3
"""Runs every tests/test_*.py against target/debug/stickBar (or `--bin PATH`).

    ./build.sh && python3 tests/run_all.py

Each test starts its own short-lived second bar instance next to the one
you're running, so expect extra bars to flash at the top of the screen
for the ~2 minutes this takes. Exit status is non-zero if any test failed
(skips don't count as failures).
"""

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    extra = sys.argv[1:]
    results = []
    for test in sorted(HERE.glob("test_*.py")):
        print(f"--- {test.name}", flush=True)
        rc = subprocess.run([sys.executable, str(test), *extra]).returncode
        results.append((test.name, {0: "PASS", 77: "SKIP"}.get(rc, "FAIL")))
    print()
    for name, status in results:
        print(f"{status:5s} {name}")
    sys.exit(1 if any(s == "FAIL" for _, s in results) else 0)


if __name__ == "__main__":
    main()
