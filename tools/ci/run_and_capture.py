#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 4 or sys.argv[2] != "--":
        print(
            "usage: run_and_capture.py <log-file> -- <command> [args...]",
            file=sys.stderr,
        )
        return 2

    log_path = pathlib.Path(sys.argv[1])
    command = sys.argv[3:]
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    log_path.write_bytes(completed.stdout)

    if completed.returncode != 0:
        sys.stdout.buffer.write(completed.stdout)
        if completed.stdout and not completed.stdout.endswith(b"\n"):
            sys.stdout.buffer.write(b"\n")
        print(
            f"command failed with exit code {completed.returncode}: "
            + " ".join(command),
            file=sys.stderr,
        )
    else:
        print("command succeeded: " + " ".join(command))

    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
