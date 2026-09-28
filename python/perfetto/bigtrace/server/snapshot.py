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
"""Parse-once snapshots of traces.

`trace_processor_shell export perfetto` writes what the shell parsed as a tar
that a shell of the same version loads several times faster than the original
trace. A snapshot holds only the parsed trace: tables created by queries are
not in it, and the metadata, stats and trace_file tables are rewritten to
describe the tar, so queries using them need the raw trace (NEEDS_RAW_TRACE).
"""

import collections
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import logging
import os
import re
import shutil
import subprocess
import tempfile
import threading
from typing import List, Optional, Sequence, Set

logger = logging.getLogger(__name__)

NEEDS_RAW_TRACE = re.compile(
    r"\b(metadata|stats|trace_file|__intrinsic_(metadata|stats|trace_file))\b",
    re.IGNORECASE)

_EXTENSIONS = (".tar.zst", ".tar.gz")
# Large traces can take minutes to parse.
_EXPORT_TIMEOUT_SECS = 1200


def _remove(path: str) -> None:
  try:
    os.remove(path)
  except OSError:
    pass


class SnapshotStore:
  """Snapshots in a directory, evicting the least recently used over budget.

  A snapshot is keyed by the trace's path, size and mtime and by the
  trace_processor_shell version, so changed traces and new shells miss.

  schedule() creates snapshots in the background with separate
  `trace_processor_shell export` processes, `workers` at a time. This parses
  the trace a second time, but holds no loaded trace while queued and copies
  no data through Python, which is far slower than the shell on many cores.
  """

  def __init__(self,
               directory: str,
               budget_bytes: int,
               shell_path: str,
               workers: int = max(1, (os.cpu_count() or 2) // 2)):
    self.directory = os.path.abspath(directory)
    self.budget_bytes = budget_bytes
    self.shell_path = shell_path
    os.makedirs(self.directory, exist_ok=True)
    self.version = subprocess.run([shell_path, "--version"],
                                  capture_output=True,
                                  text=True,
                                  check=True,
                                  timeout=60).stdout.strip()
    self._zstd = shutil.which("zstd")
    self._lock = threading.Lock()
    self._cond = threading.Condition(self._lock)
    self._paused = 0
    self._workers = ThreadPoolExecutor(max_workers=workers)
    self._pending: Set[str] = set()
    self._procs: Set[subprocess.Popen] = set()
    self._closed = False
    # Snapshot path to size, least recently used first.
    self._sizes: "collections.OrderedDict[str, int]" = (
        collections.OrderedDict())
    found = []
    for name in os.listdir(self.directory):
      path = os.path.join(self.directory, name)
      if name.startswith("."):
        _remove(path)  # A temp file left by an interrupted export.
      elif name.endswith(_EXTENSIONS):
        st = os.stat(path)
        found.append((st.st_mtime, path, st.st_size))
    for _, path, size in sorted(found):
      self._sizes[path] = size
    with self._lock:
      evicted = self._evict_locked()
    for old in evicted:
      _remove(old)

  def close(self) -> None:
    """Drops queued snapshots and stops the running ones."""
    with self._lock:
      self._closed = True
      self._cond.notify_all()
      procs = list(self._procs)
    for p in procs:
      p.kill()
    self._workers.shutdown(wait=True, cancel_futures=True)

  def total_bytes(self) -> int:
    with self._lock:
      return sum(self._sizes.values())

  def _key(self, trace_path: str) -> str:
    st = os.stat(trace_path)
    ident = "\0".join([
        os.path.abspath(trace_path),
        str(st.st_size),
        str(st.st_mtime_ns), self.version
    ])
    return hashlib.sha256(ident.encode("utf-8")).hexdigest()[:32]

  def find(self, trace_path: str) -> Optional[str]:
    """Returns the trace's snapshot, if any, marking it as recently used."""
    try:
      key = self._key(trace_path)
    except OSError:
      return None
    with self._lock:
      for ext in _EXTENSIONS:
        path = os.path.join(self.directory, key + ext)
        if path in self._sizes:
          self._sizes.move_to_end(path)
          try:
            os.utime(path)  # The mtime orders snapshots across restarts.
          except OSError:
            pass
          return path
    return None

  def discard(self, snapshot_path: str) -> None:
    with self._lock:
      self._sizes.pop(snapshot_path, None)
    _remove(snapshot_path)

  def pause(self) -> None:
    """Holds back new snapshots until the matching resume()."""
    with self._lock:
      self._paused += 1

  def resume(self) -> None:
    with self._lock:
      self._paused -= 1
      self._cond.notify_all()

  def schedule(self, trace_path: str, shell_flags: Sequence[str] = ()) -> None:
    """Creates the trace's snapshot in the background unless it has one.

    Snapshots are only started while not paused: creating them alongside a
    query over many traces slows the query down (each export parses the trace
    again and writes a large tar).
    """
    with self._lock:
      if self._closed or trace_path in self._pending:
        return
      self._pending.add(trace_path)
    self._workers.submit(self._create_if_missing, trace_path, list(shell_flags))

  def _create_if_missing(self, trace_path: str, shell_flags: List[str]) -> None:
    try:
      with self._lock:
        self._cond.wait_for(lambda: self._closed or not self._paused)
        if self._closed:
          return
      if not self.find(trace_path):
        self.create(trace_path, shell_flags)
    except (OSError, subprocess.SubprocessError) as e:
      if not self._closed:
        logger.warning("Snapshot of %s failed: %s", trace_path, e)
    finally:
      with self._lock:
        self._pending.discard(trace_path)
        self._cond.notify_all()

  def wait(self) -> None:
    """Waits until every scheduled snapshot is done (needs no query running)."""
    with self._lock:
      self._cond.wait_for(lambda: self._closed or not self._pending)

  def create(self, trace_path: str, shell_flags: Sequence[str] = ()) -> str:
    """Exports the trace as its snapshot and returns the snapshot's path.

    `shell_flags` must match the flags queries load traces with. Compresses
    with the zstd command if installed, else with gzip. Readers never see a
    partial snapshot: it is written to temp files and renamed.
    """
    ext = ".tar.zst" if self._zstd else ".tar.gz"
    path = os.path.join(self.directory, self._key(trace_path) + ext)
    fd, tar = tempfile.mkstemp(dir=self.directory, prefix=".", suffix=".tar")
    os.close(fd)
    try:
      self._run([
          self.shell_path, "export", "perfetto", *shell_flags, "--quiet", "-o",
          tar, trace_path
      ])
      if self._zstd:
        self._run([self._zstd, "-3", "-T0", "-q", "-f", tar, "-o", tar + ext])
      else:
        with open(tar, "rb") as src, gzip.open(tar + ext, "wb", 3) as dst:
          shutil.copyfileobj(src, dst, 1 << 20)
      os.replace(tar + ext, path)
    finally:
      _remove(tar)
      _remove(tar + ext)
    with self._lock:
      self._sizes[path] = os.path.getsize(path)
      self._sizes.move_to_end(path)
      evicted = self._evict_locked()
    for old in evicted:
      _remove(old)
    return path

  def _run(self, args: List[str]) -> None:
    """Runs a command that close() can kill; raises if it fails."""
    with self._lock:
      if self._closed:
        raise subprocess.SubprocessError("snapshot store closed")
      p = subprocess.Popen(
          args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
      self._procs.add(p)
    try:
      _, stderr = p.communicate(timeout=_EXPORT_TIMEOUT_SECS)
    except subprocess.TimeoutExpired:
      p.kill()
      p.communicate()
      raise
    finally:
      with self._lock:
        self._procs.discard(p)
    if p.returncode:
      raise subprocess.SubprocessError(
          f"{os.path.basename(args[0])} exited with {p.returncode}: "
          f"{stderr.decode('utf-8', 'replace').strip()[-500:]}")

  def _evict_locked(self) -> List[str]:
    """Drops the least recently used snapshots over budget, but not the last.

    Returns the files to delete.
    """
    evicted = []
    total = sum(self._sizes.values())
    while total > self.budget_bytes and len(self._sizes) > 1:
      old, size = self._sizes.popitem(last=False)
      evicted.append(old)
      total -= size
    return evicted
