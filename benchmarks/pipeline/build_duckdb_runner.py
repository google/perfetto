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
"""Build the optional standalone native DuckDB benchmark runner.

Supply an unpacked official, explicitly versioned DuckDB C-library release.
Tested with v1.4.4 (libduckdb-linux-amd64.zip), downloaded separately from:
https://github.com/duckdb/duckdb/releases/tag/v1.4.4
The directory must contain duckdb.h and libduckdb.so (or libduckdb.dylib).
No files are downloaded, and DuckDB is not added to Perfetto's dependencies.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--duckdb-dir', type=Path, required=True)
  parser.add_argument('--output', type=Path, required=True)
  parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
  args = parser.parse_args()
  library_dir = args.duckdb_dir.resolve()
  if not (library_dir / 'duckdb.h').is_file():
    parser.error('--duckdb-dir must contain duckdb.h')
  library = next((library_dir / name
                  for name in ('libduckdb.so', 'libduckdb.dylib')
                  if (library_dir / name).is_file()), None)
  if library is None:
    parser.error('--duckdb-dir must contain libduckdb.so or libduckdb.dylib')
  args.output.parent.mkdir(parents=True, exist_ok=True)
  compiler = shlex.split(args.cxx)
  source = Path(__file__).resolve().with_name('duckdb_runner.cc')
  command = compiler + [
      '-std=c++17', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Werror',
      '-I' + str(library_dir),
      str(source), '-L' + str(library_dir), '-Wl,-rpath,' + str(library_dir),
      '-lduckdb', '-o',
      str(args.output)
  ]
  subprocess.run(command, check=True)
  manifest = {
      'compile_command':
          command,
      'compiler_version':
          subprocess.check_output(compiler + ['--version'], text=True),
      'library_path':
          str(library),
      'library_sha256':
          hashlib.sha256(library.read_bytes()).hexdigest(),
      'header_sha256':
          hashlib.sha256((library_dir / 'duckdb.h').read_bytes()).hexdigest(),
      'source_sha256':
          hashlib.sha256(source.read_bytes()).hexdigest(),
      'protocol_sha256':
          hashlib.sha256(source.with_name('protocol.h').read_bytes()
                        ).hexdigest(),
      'runner_sha256':
          hashlib.sha256(args.output.read_bytes()).hexdigest(),
  }
  Path(str(args.output) +
       '.build.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
  main()
