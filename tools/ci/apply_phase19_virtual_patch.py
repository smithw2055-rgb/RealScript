#!/usr/bin/env python3

# The patch is staged as eight ordered, hash-verified base64 fragments.
import base64
import hashlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PARTS = sorted((ROOT / "tools" / "ci").glob("phase19_virtual_patch_*.b64"))
EXPECTED_SHA256 = "714bbe2cefae3fa4823bdecb7edb743d8854d93ea303d9016c9f792b6b5fe58d"
EXPECTED_PARTS = [
    (8000, "81ada3aae761bd7f20bb728ee00362030b3ea28127a6010ec373cf61f2abd5c8"),
    (8000, "2616ce8935c81405c9af6f52e3f7c05efb2e637bc6b3f172adafd9fc5932ed77"),
    (8000, "810069664b59ec5032c0f8df6682346b2d169d2d88e45f6f0d1a1d2353961860"),
    (8000, "9d7046a685c065fbcd17f96bfdc24018bfa76e2a566104b2057839a2933e6710"),
    (8000, "cfdabe263a39faef7d06507456f6a5feefaaa26f5709820fd196915f1359a714"),
    (8000, "fea366755cacb313e4277229a66396aa50dea5a79058c4e33a8ae305e41de5b1"),
    (8000, "cde2add819ec45a6a551006dd31298fc603958667ce0cb36ef31682633ed6e94"),
    (3906, "8e1c9e6a5757a96e06c564a8de6a4e12891c042c3c3640444f7f12d1e3447565"),
]

if len(PARTS) != len(EXPECTED_PARTS):
    raise RuntimeError(f"expected {len(EXPECTED_PARTS)} patch parts, found {len(PARTS)}")

decoded_parts = []
part_errors = []
for index, (path, (expected_size, expected_hash)) in enumerate(zip(PARTS, EXPECTED_PARTS)):
    data = base64.b64decode(path.read_text(encoding="utf-8").strip(), validate=True)
    actual_hash = hashlib.sha256(data).hexdigest()
    print(f"part {index:02d}: bytes={len(data)} sha256={actual_hash}")
    if len(data) != expected_size or actual_hash != expected_hash:
        part_errors.append(
            f"part {index:02d} expected bytes={expected_size} sha256={expected_hash}")
    decoded_parts.append(data)

if part_errors:
    raise RuntimeError("Phase 19 patch fragment mismatch:\n" + "\n".join(part_errors))

patch = b"".join(decoded_parts)
actual = hashlib.sha256(patch).hexdigest()
print(f"combined: bytes={len(patch)} sha256={actual}")
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
