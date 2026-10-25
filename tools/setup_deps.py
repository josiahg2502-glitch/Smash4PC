#!/usr/bin/env python3
"""Fetch the pinned sources the build needs into third_party/."""

import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def git(directory, *args, capture=False, check=True):
    return subprocess.run(["git", "-C", str(directory), *args], check=check,
                          capture_output=capture, text=True)


def main():
    dependencies = json.loads((ROOT / "dependencies.lock.json").read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("names", nargs="*", help="Default: all of dependencies.lock.json")
    args = parser.parse_args()
    selected = args.names or list(dependencies)
    for name in selected:
        if name not in dependencies:
            parser.error(f"Unknown dependency: {name}")
    for name in selected:
        dependency = dependencies[name]
        directory = ROOT / "third_party" / name
        if not directory.exists():
            directory.mkdir(parents=True)
            git(directory, "init", "--quiet")
            git(directory, "remote", "add", "origin", dependency["url"])
            git(directory, "fetch", "--depth", "1", "origin", dependency["revision"])
            git(directory, "checkout", "--detach", "FETCH_HEAD")
        actual = git(directory, "rev-parse", "HEAD", capture=True).stdout.strip()
        if actual != dependency["revision"]:
            parser.exit(1, f"Refusing to change existing {directory}: expected {dependency['revision']}, got {actual}\n")
        print(f"{name}: {actual}")


if __name__ == "__main__":
    main()
