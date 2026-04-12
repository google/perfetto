#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Renders every preset in test/data/music_synth_presets.json to a WAV file.

For each preset:
  1. Convert its `patch` dict to a binary SynthPatch proto via protoc-compiled
     Python bindings + google.protobuf.json_format.ParseDict.
  2. Write the binary to a temp file.
  3. Invoke trace_processor_shell with:
        techno --patch-file <temp> --duration-secs <N> -o out/preset_wavs/NAME.wav
  4. Report success/failure.

The synth.proto is compiled to Python on the fly at startup using the protoc
binary from the build directory.

Dependencies: python3-protobuf (pip install protobuf) and a built TP shell.
"""

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PRESETS = REPO_ROOT / "test" / "data" / "music_synth_presets.json"
DEFAULT_OUT_DIR = REPO_ROOT / "out" / "preset_wavs"
DEFAULT_DURATION_SECS = 16.0


def _find_protoc() -> Path:
  # Prefer the currently-used OUT dir if $OUT is set, else pick the freshest.
  env_out = os.environ.get("OUT", "").strip()
  candidates: list[Path] = []
  if env_out:
    candidates.append(REPO_ROOT / env_out / "protoc")
  out_dir = REPO_ROOT / "out"
  if out_dir.is_dir():
    for child in sorted(
        out_dir.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True):
      p = child / "protoc"
      if p.is_file():
        candidates.append(p)
  for c in candidates:
    if c.is_file():
      return c
  raise RuntimeError("Could not find protoc under out/*/protoc")


def _find_trace_processor_shell() -> Path:
  env_out = os.environ.get("OUT", "").strip()
  candidates: list[Path] = []
  if env_out:
    candidates.append(REPO_ROOT / env_out / "trace_processor_shell")
  for child in sorted(
      (REPO_ROOT / "out").iterdir(),
      key=lambda p: p.stat().st_mtime,
      reverse=True):
    p = child / "trace_processor_shell"
    if p.is_file():
      candidates.append(p)
  for c in candidates:
    if c.is_file():
      return c
  raise RuntimeError("Could not find trace_processor_shell under out/*/")


def _compile_synth_proto(protoc: Path) -> Path:
  """Compiles synth.proto into a temporary Python module directory and
  returns that directory (to be prepended to sys.path)."""
  out_dir = Path(tempfile.mkdtemp(prefix="synth_py_"))
  proto_src = REPO_ROOT / "protos" / "perfetto" / "trace_processor" / "synth.proto"
  if not proto_src.is_file():
    raise RuntimeError(f"synth.proto not found at {proto_src}")

  # protoc -I=ROOT --python_out=OUT protos/.../synth.proto
  cmd = [
      str(protoc),
      f"--proto_path={REPO_ROOT}",
      f"--python_out={out_dir}",
      str(proto_src.relative_to(REPO_ROOT)),
  ]
  subprocess.run(cmd, check=True, cwd=REPO_ROOT)
  return out_dir


def _render_one(preset: dict, patch_module, shell: Path, out_dir: Path,
                duration_secs: float, index: int) -> tuple[str, bool, str]:
  """Renders a single preset. Returns (name, ok, error_message)."""
  from google.protobuf import json_format  # pylint: disable=import-outside-toplevel

  name = preset["name"]
  wav_path = out_dir / f"{index:03d}_{name}.wav"

  # Build binary SynthPatch.
  patch_proto = patch_module.SynthPatch()
  try:
    json_format.ParseDict(
        preset["patch"], patch_proto, ignore_unknown_fields=False)
  except json_format.ParseError as e:
    return (name, False, f"JSON→proto: {e}")

  binary = patch_proto.SerializeToString()
  with tempfile.NamedTemporaryFile(suffix=".pb", delete=False) as tf:
    tf.write(binary)
    patch_file = Path(tf.name)

  try:
    cmd = [
        str(shell),
        "techno",
        "--patch-file",
        str(patch_file),
        "--duration-secs",
        str(duration_secs),
        "-o",
        str(wav_path),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
      return (name, False, f"trace_processor_shell exit {result.returncode}: "
              f"{result.stderr.strip()}")
    return (name, True, "")
  finally:
    try:
      patch_file.unlink()
    except OSError:
      pass


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument(
      "--presets",
      type=Path,
      default=DEFAULT_PRESETS,
      help="Input presets JSON file")
  parser.add_argument(
      "--out-dir",
      type=Path,
      default=DEFAULT_OUT_DIR,
      help="Output WAV directory")
  parser.add_argument(
      "--duration-secs",
      type=float,
      default=DEFAULT_DURATION_SECS,
      help="Render duration per preset")
  parser.add_argument(
      "--shell",
      type=Path,
      default=None,
      help="Path to trace_processor_shell binary")
  parser.add_argument(
      "--filter", type=str, default="", help="Substring filter on preset name")
  parser.add_argument(
      "--jobs",
      type=int,
      default=os.cpu_count() or 4,
      help="Number of parallel render jobs")
  args = parser.parse_args()

  # Locate binaries.
  shell = args.shell or _find_trace_processor_shell()
  protoc = _find_protoc()
  print(f"Using trace_processor_shell: {shell}")
  print(f"Using protoc:                {protoc}")

  # Compile the proto and import it.
  py_dir = _compile_synth_proto(protoc)
  sys.path.insert(0, str(py_dir))
  # The compiled module is at protos/perfetto/trace_processor/synth_pb2.py.
  from protos.perfetto.trace_processor import synth_pb2  # type: ignore

  # Load presets.
  with args.presets.open() as f:
    doc = json.load(f)
  presets = doc.get("presets", [])
  if args.filter:
    presets = [p for p in presets if args.filter in p["name"]]
  print(f"Loaded {len(presets)} presets from {args.presets}")

  args.out_dir.mkdir(parents=True, exist_ok=True)

  # Render in parallel.
  results: list[tuple[str, bool, str]] = []
  with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    futures = []
    for idx, preset in enumerate(presets):
      futures.append(
          pool.submit(_render_one, preset, synth_pb2, shell, args.out_dir,
                      args.duration_secs, idx))
    for i, fut in enumerate(concurrent.futures.as_completed(futures)):
      name, ok, err = fut.result()
      status = "OK" if ok else "FAIL"
      print(f"  [{i+1:3d}/{len(presets)}] {status} {name}" +
            (f"  ({err})" if not ok else ""))
      results.append((name, ok, err))

  n_ok = sum(1 for _, ok, _ in results if ok)
  n_fail = len(results) - n_ok
  print(f"\nDone: {n_ok} ok, {n_fail} failed")
  if n_fail:
    print("\nFailed presets:")
    for name, ok, err in results:
      if not ok:
        print(f"  - {name}: {err}")
    return 1
  return 0


if __name__ == "__main__":
  sys.exit(main())
