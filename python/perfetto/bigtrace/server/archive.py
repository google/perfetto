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
"""Unpacks zip and tar archives of traces so they can be served."""

import hashlib
import os
import shutil
import tarfile
import zipfile
from typing import IO, Iterator, Tuple

DEFAULT_CACHE_DIR = os.path.join(
    os.path.expanduser("~"), ".cache", "perfetto", "bigtrace")


def is_archive(path: str) -> bool:
  """True for zip and tar files (any compression), judged by content."""
  if not os.path.isfile(path):
    return False
  return zipfile.is_zipfile(path) or tarfile.is_tarfile(path)


def extract_archive(path: str, cache_dir: str = DEFAULT_CACHE_DIR) -> str:
  """Extracts `path` once and returns the directory holding its files.

  The directory is keyed by the archive's path, size and mtime, so restarts
  reuse it (with its manifest and query history) until the archive changes.
  """
  path = os.path.abspath(path)
  st = os.stat(path)
  key = hashlib.sha1(
      f"{path}:{st.st_size}:{st.st_mtime_ns}".encode()).hexdigest()[:12]
  out_dir = os.path.join(
      os.path.abspath(cache_dir), f"{os.path.basename(path)}-{key}")
  if os.path.isdir(out_dir):
    return out_dir

  tmp_dir = f"{out_dir}.tmp"
  shutil.rmtree(tmp_dir, ignore_errors=True)
  os.makedirs(tmp_dir)
  for name, src in _members(path):
    dest = os.path.normpath(os.path.join(tmp_dir, name))
    # Skip absolute paths and ".." entries that would escape tmp_dir.
    if not dest.startswith(tmp_dir + os.sep):
      continue
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with src, open(dest, "wb") as dst:
      shutil.copyfileobj(src, dst)
  os.rename(tmp_dir, out_dir)
  return out_dir


def _members(path: str) -> Iterator[Tuple[str, IO[bytes]]]:
  """Yields (name, file object) for each regular file in the archive."""
  if zipfile.is_zipfile(path):
    with zipfile.ZipFile(path) as zf:
      for info in zf.infolist():
        if not info.is_dir():
          yield info.filename, zf.open(info)
    return
  with tarfile.open(path) as tf:
    for member in tf:
      if member.isfile():
        yield member.name, tf.extractfile(member)
