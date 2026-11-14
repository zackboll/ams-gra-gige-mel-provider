#!/usr/bin/env python3
"""Run one internal Pleora program with the selected SDK's process environment.

Usage: python3 scripts/run_pleora.py SDK_ROOT PROGRAM [ARGUMENT ...]

eBUS's native loader needs GENICAM_ROOT_V3_4 before main(), even for --help.
This launcher changes only the child process environment, not the shell or
system setup. It downloads nothing and installs no drivers or libraries.
"""

import argparse
import os
from pathlib import Path
import sys

from provision_pleora import verify_sdk


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sdk_root", type=Path)
    parser.add_argument("program")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    sdk = args.sdk_root.expanduser().resolve()
    verify_sdk(sdk)
    cache_root = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
    cache = cache_root / "gige-mel" / "genicam-v3_4"
    cache.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.update({
        "PUREGEV_ROOT": str(sdk),
        "GENICAM_ROOT": str(sdk / "lib/genicam"),
        "GENICAM_ROOT_V3_4": str(sdk / "lib/genicam"),
        "GENICAM_CACHE": str(cache),
        "GENICAM_CACHE_V3_4": str(cache),
    })
    paths = [str(sdk / "lib"), str(sdk / "lib/genicam/bin/Linux64_x64")]
    if environment.get("LD_LIBRARY_PATH"):
        paths.append(environment["LD_LIBRARY_PATH"])
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(paths)
    os.execvpe(args.program, [args.program, *args.arguments], environment)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"run_pleora: {error}", file=sys.stderr)
        raise SystemExit(1)
