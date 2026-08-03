#!/usr/bin/env python3

import base64
import hashlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PARTS = sorted((ROOT / "tools" / "ci").glob("phase19_virtual_patch_*.b64"))
EXPECTED_SHA256 = "714bbe2cefae3fa4823bdecb7edb743d8854d93ea303d9016c9f792b6b5fe58d"

if len(PARTS) != 8:
    raise RuntimeError(f"expected 8 patch parts, found {len(PARTS)}")

encoded = "".join(path.read_text(encoding="utf-8").strip() for path in PARTS)
patch = base64.b64decode(encoded, validate=True)
actual = hashlib.sha256(patch).hexdigest()
if actual != EXPECTED_SHA256:
    raise RuntimeError(f"Phase 19 patch hash mismatch: {actual}")

patch_path = ROOT / ".phase19-virtual.patch"
patch_path.write_bytes(patch)
subprocess.run(["git", "apply", "--check", str(patch_path)], cwd=ROOT, check=True)
subprocess.run(["git", "apply", str(patch_path)], cwd=ROOT, check=True)
patch_path.unlink()

for path in PARTS:
    path.unlink()

standard_workflow = '''name: RealScript CI

on:
  push:
    branches:
      - main
  pull_request:

jobs:
  build-and-test:
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-latest, windows-latest]

    runs-on: ${{ matrix.os }}

    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Configure
        run: >-
          python tools/ci/run_and_capture.py configure.log --
          cmake -S . -B build
          -DREALSCRIPT_BUILD_TESTS=ON
          -DREALSCRIPT_WARNINGS_AS_ERRORS=ON

      - name: Build
        run: >-
          python tools/ci/run_and_capture.py build.log --
          cmake --build build --config Debug

      - name: Test
        run: >-
          python tools/ci/run_and_capture.py test.log --
          ctest --test-dir build -C Debug --output-on-failure

      - name: Upload CI failure logs
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: ci-log-${{ matrix.os }}
          path: |
            configure.log
            build.log
            test.log
            build/Testing/Temporary/LastTest.log
          if-no-files-found: ignore
'''
(ROOT / ".github" / "workflows" / "ci.yml").write_text(
    standard_workflow, encoding="utf-8")

Path(__file__).unlink()
