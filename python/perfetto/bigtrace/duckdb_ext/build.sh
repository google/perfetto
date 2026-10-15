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

# Builds perfetto.duckdb_extension (C_STRUCT ABI, DuckDB >= v1.5.6) for Linux.
# Usage: build.sh [OUT]. Downloads the two DuckDB headers into $DUCKDB_SRC.
set -euo pipefail
DUCKDB_VERSION=v1.5.6
OUT="$(realpath -m "${1:-$(dirname "$0")/perfetto.duckdb_extension}")"
cd "$(dirname "$0")"
case "$(uname -m)" in
  x86_64) PLATFORM=linux_amd64 ;;
  aarch64 | arm64) PLATFORM=linux_arm64 ;;
  *) echo "unsupported platform $(uname -m)" >&2; exit 1 ;;
esac
INC="${DUCKDB_SRC:-third_party/duckdb}/$DUCKDB_VERSION"
mkdir -p "$INC" "$(dirname "$OUT")"
for h in duckdb.h duckdb_extension.h; do
  [ -f "$INC/$h" ] || curl -fsSL -o "$INC/$h.$$" \
    "https://raw.githubusercontent.com/duckdb/duckdb/$DUCKDB_VERSION/src/include/$h"
  [ -f "$INC/$h" ] || mv "$INC/$h.$$" "$INC/$h"
done
TMP="$OUT.$$.tmp"
trap 'rm -f "$TMP"' EXIT
"${CXX:-g++}" -O3 -std=c++17 -fPIC -fvisibility=hidden -pthread -Wall -Wextra \
  -Werror -isystem "$INC" -shared -static-libstdc++ -static-libgcc \
  src/*.cc -o "$TMP"
# DuckDB wants a 512-byte metadata footer behind a custom section header, as
# duckdb/extension-ci-tools' append_extension_metadata.py writes it.
field() { printf '%s' "$1"; head -c $((32 - ${#1})) /dev/zero; }
{
  printf '\x00\x93\x04\x10duckdb_signature\x80\x04'
  field ""; field ""; field ""  # FIELD8-6: unused.
  field C_STRUCT                # FIELD5: ABI type.
  field v0.0.1                  # FIELD4: extension version.
  field "$DUCKDB_VERSION"       # FIELD3: minimum C API version.
  field "$PLATFORM"             # FIELD2: platform.
  field 4                       # FIELD1: magic.
  head -c 256 /dev/zero         # Signature (unsigned).
} >> "$TMP"
mv -f "$TMP" "$OUT"
