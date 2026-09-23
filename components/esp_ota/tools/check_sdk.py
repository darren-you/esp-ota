#!/usr/bin/env python3
"""Verify the exact public IDF/lwIP source combination used by esp-ota."""

import argparse
import json
import subprocess
from pathlib import Path


LOCK = json.loads((Path(__file__).resolve().parent.parent / "sdk-lock.json").read_text())


def git(path: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(path), *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.rstrip("\n")


def check(path: Path) -> None:
    idf = path.resolve()
    lwip = idf / LOCK["lwip"]["path"]
    if git(idf, "rev-parse", "HEAD") != LOCK["idf"]["revision"]:
        raise ValueError("ESP-IDF commit differs from sdk-lock.json")
    if not lwip.is_dir() or git(lwip, "rev-parse", "HEAD") != LOCK["lwip"]["revision"]:
        raise ValueError("ESP lwIP commit differs from sdk-lock.json")
    if git(lwip, "status", "--porcelain", "--untracked-files=no"):
        raise ValueError("ESP lwIP worktree has tracked changes")
    status = git(idf, "status", "--porcelain", "--untracked-files=no")
    if status.splitlines() != [f" M {LOCK['lwip']['path']}"]:
        raise ValueError("ESP-IDF worktree differs beyond the locked lwIP gitlink")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", required=True, type=Path)
    args = parser.parse_args()
    try:
        check(args.path)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f"esp-ota SDK check failed: {error}\n")
    print("esp-ota SDK source matches sdk-lock.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
