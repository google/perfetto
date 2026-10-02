#!/bin/bash
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

# Builds out/duckdb_ext/perfetto.duckdb_extension.
# Usage: contrib/duckdb-perfetto/build.sh [out_dir]

set -euo pipefail

# The DuckDB release the extension is built for. The C++ extension ABI is not
# stable: the extension only loads into this exact DuckDB version.
DUCKDB_VERSION=v1.5.6

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/contrib/duckdb-perfetto"
OUT="${1:-out/duckdb_ext}"
cd "$ROOT"

# Headers only: a sparse, shallow checkout of the DuckDB source tree.
SRC="$HERE/third_party/duckdb"
if [ ! -f "$SRC/.perfetto_version" ] || \
   [ "$(cat "$SRC/.perfetto_version")" != "$DUCKDB_VERSION" ]; then
  rm -rf "$SRC"
  git clone --quiet --depth 1 --branch "$DUCKDB_VERSION" --filter=blob:none \
    --sparse https://github.com/duckdb/duckdb.git "$SRC"
  git -C "$SRC" sparse-checkout set src/include third_party
  echo "$DUCKDB_VERSION" > "$SRC/.perfetto_version"
fi

if [ ! -f "$OUT/args.gn" ]; then
  mkdir -p "$OUT"
  echo 'is_debug = false' > "$OUT/args.gn"
fi
tools/gn gen --check "$OUT" --root=. --dotfile=contrib/duckdb-perfetto/.gn
tools/ninja -C "$OUT" perfetto_duckdb

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) PLATFORM=osx_arm64 ;;
  Darwin-x86_64) PLATFORM=osx_amd64 ;;
  Linux-x86_64) PLATFORM=linux_amd64 ;;
  Linux-aarch64) PLATFORM=linux_arm64 ;;
  *) echo "Unsupported platform"; exit 1 ;;
esac

# GN's standalone toolchain uses .so on both Linux and macOS.
"$HERE/package_extension.py" --library "$OUT/libperfetto_duckdb.so" \
  --out "$OUT/perfetto.duckdb_extension" \
  --platform "$PLATFORM" --duckdb-version "$DUCKDB_VERSION" --abi-type CPP
echo "Built $OUT/perfetto.duckdb_extension"
