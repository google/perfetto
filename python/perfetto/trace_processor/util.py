# Copyright (C) 2022 The Android Open Source Project
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

from typing import BinaryIO, Dict, Optional, Tuple

# Limit parsing file to 32MB to maintain parity with the UI
MAX_BYTES_LOADED = 32 * 1024 * 1024


def file_generator(path: str):
  with open(path, 'rb') as f:
    yield from read_generator(f)


def read_generator(trace: BinaryIO):
  while True:
    chunk = trace.read(MAX_BYTES_LOADED)
    if not chunk:
      break
    yield chunk


def merge_dicts(a: Dict[str, str], b: Dict[str, str]):
  return {**a, **b}


def parse_trace_uri(uri: str) -> Tuple[Optional[str], str]:
  # This is definitely a path and not a URI
  if uri.startswith('/') or uri.startswith('.'):
    return None, uri

  # If there's no colon, it cannot be a URI
  idx = uri.find(':')
  if idx == -1:
    return None, uri

  return (uri[:idx], uri[idx + 1:])
