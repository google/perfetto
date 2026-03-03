#!/usr/bin/env python3
"""Benchmark compression on trace files with various compressors and parameters.

Compresses input files using zstd/xz/bzip2 at multiple compression levels and
block sizes (plus full-file), then reports the results in tabular format.

Usage:
  python3 tools/compression_benchmark.py wakelock.pftrace wakelock_uninterned.pftrace
"""

import os
import subprocess
import sys
import tempfile
import time


def compress_blocks(input_path, compressor, level, block_size_bytes):
  """Compress a file block-by-block and return total compressed size.

    Each block is compressed independently.  For full-file mode pass
    block_size_bytes=None.
    """
  input_data = open(input_path, 'rb').read()
  input_size = len(input_data)

  if block_size_bytes is None:
    blocks = [input_data]
  else:
    blocks = [
        input_data[i:i + block_size_bytes]
        for i in range(0, input_size, block_size_bytes)
    ]

  total_compressed = 0

  with tempfile.NamedTemporaryFile(suffix='.cmp', delete=True) as tmp_out, \
       tempfile.NamedTemporaryFile(suffix='.bin', delete=True) as tmp_in:
    for block in blocks:
      tmp_in.seek(0)
      tmp_in.truncate()
      tmp_in.write(block)
      tmp_in.flush()

      if compressor == 'zstd':
        cmd = ['zstd', f'-{level}', '-f', '-o', tmp_out.name, tmp_in.name]
      elif compressor == 'xz':
        cmd = ['xz', f'-{level}', '--keep', '--force', '--stdout', tmp_in.name]
      elif compressor == 'bzip2':
        cmd = [
            'bzip2', f'-{level}', '--keep', '--force', '--stdout', tmp_in.name
        ]
      else:
        raise ValueError(f"Unknown compressor: {compressor}")

      if compressor in ('xz', 'bzip2'):
        with open(tmp_out.name, 'wb') as out_f:
          result = subprocess.run(cmd, stdout=out_f, stderr=subprocess.PIPE)
      else:
        result = subprocess.run(cmd, capture_output=True)

      if result.returncode != 0:
        print(f"{compressor} error: {result.stderr.decode()}", file=sys.stderr)
        return None

      total_compressed += os.path.getsize(tmp_out.name)

  return total_compressed


def format_size(size_bytes):
  if size_bytes < 1024:
    return f"{size_bytes} B"
  elif size_bytes < 1024 * 1024:
    return f"{size_bytes / 1024:.1f} KB"
  else:
    return f"{size_bytes / (1024 * 1024):.2f} MB"


def main():
  if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <file1> <file2> [...]", file=sys.stderr)
    sys.exit(1)

  input_files = sys.argv[1:]

  # (compressor, level, block_size_bytes, block_label)
  configs = []

  block_sizes = [
      (512 * 1024, "512K"),
      (1 * 1024 * 1024, "1MB"),
      (2 * 1024 * 1024, "2MB"),
      (8 * 1024 * 1024, "8MB"),
      (None, "full"),
  ]

  for comp in ['zstd', 'xz', 'bzip2']:
    if comp == 'zstd':
      levels = [10, 15, 19]
    elif comp == 'xz':
      levels = [6, 9]  # xz default is 6, max useful is 9
    elif comp == 'bzip2':
      levels = [9]  # bzip2 -9 is the standard choice
    for level in levels:
      for bs_bytes, bs_label in block_sizes:
        configs.append((comp, level, bs_bytes, bs_label))

  # results[fname][(comp, level, bs_label)] = compressed_size
  results = {}
  file_sizes = {}

  for input_path in input_files:
    fname = os.path.basename(input_path)
    file_size = os.path.getsize(input_path)
    file_sizes[fname] = file_size
    results[fname] = {}
    print(f"\nBenchmarking: {fname} ({format_size(file_size)})")

    for comp, level, bs_bytes, bs_label in configs:
      config_key = (comp, level, bs_label)
      label = f"{comp}-{level} / {bs_label}"
      sys.stdout.write(f"  {label:<22} ")
      sys.stdout.flush()

      t0 = time.time()
      compressed = compress_blocks(input_path, comp, level, bs_bytes)
      elapsed = time.time() - t0

      results[fname][config_key] = compressed
      ratio = compressed / file_size if compressed else 0
      print(f"{format_size(compressed):>10}  "
            f"({ratio:.4f}x)  [{elapsed:.1f}s]")

  # ---- Summary table ----
  print("\n" + "=" * 90)
  print("COMPRESSION BENCHMARK RESULTS")
  print("=" * 90)

  fnames = [os.path.basename(f) for f in input_files]

  # Column widths
  cfg_w = 22
  col_w = 20

  # Header row 1: file names
  print(f"{'':>{cfg_w}}", end="")
  for fn in fnames:
    short = fn[:col_w]
    print(f" {short:>{col_w}}", end="")
  if len(fnames) == 2:
    print(f" {'ratio':>{col_w}}", end="")
  print()

  # Header row 2: Size / Ratio
  print(f"{'Config':<{cfg_w}}", end="")
  for _ in fnames:
    print(f" {'Size':>9} {'Ratio':>9}", end="")
  if len(fnames) == 2:
    print(f" {'unintern/intern':>{col_w}}", end="")
  print()
  print("-" * (cfg_w + (col_w + 1) * (len(fnames) +
                                      (1 if len(fnames) == 2 else 0))))

  # Original row
  print(f"{'Original':<{cfg_w}}", end="")
  for fn in fnames:
    s = file_sizes[fn]
    print(f" {format_size(s):>9} {'1.0000':>9}", end="")
  if len(fnames) == 2:
    r = file_sizes[fnames[1]] / file_sizes[fnames[0]]
    print(f" {r:>18.2f}x", end="")
  print()

  prev_comp = None
  for comp, level, bs_bytes, bs_label in configs:
    config_key = (comp, level, bs_label)
    label = f"{comp}-{level} / {bs_label}"

    # Separator between compressors
    if prev_comp is not None and comp != prev_comp:
      print()
    prev_comp = comp

    print(f"{label:<{cfg_w}}", end="")
    vals = []
    for fn in fnames:
      c = results[fn][config_key]
      vals.append(c)
      orig = file_sizes[fn]
      ratio = c / orig
      print(f" {format_size(c):>9} {ratio:>8.4f}x", end="")
    if len(fnames) == 2:
      r = vals[1] / vals[0] if vals[0] else 0
      print(f" {r:>18.2f}x", end="")
    print()


if __name__ == '__main__':
  main()
