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
"""Hands out a TraceProcessor per trace, keeping loaded ones between queries."""

import collections
from concurrent.futures import ThreadPoolExecutor
import dataclasses as dc
import itertools
import logging
import re
import threading
from typing import Iterable, List, Optional, Set
import weakref

from perfetto.trace_processor.api import (
    TraceProcessor,
    TraceProcessorConfig,
    TraceProcessorException,
)
from perfetto.bigtrace.server import query_result
from perfetto.bigtrace.server.snapshot import NEEDS_RAW_TRACE, SnapshotStore

logger = logging.getLogger(__name__)

# Large traces can take minutes to parse.
_LOAD_TIMEOUT_SECS = 600


def load_trace(
    trace_path: str,
    tp_config: TraceProcessorConfig,
    cancel_event: Optional[threading.Event] = None,
) -> TraceProcessor:
  """Starts a TraceProcessor with the trace loaded.

  The shell reads the file itself: streaming it through the Python API costs
  Python CPU time per chunk, which serializes parallel loads on the GIL.
  """
  config = dc.replace(
      tp_config,
      cancel_event=cancel_event,
      extra_flags=list(tp_config.extra_flags or []) + [trace_path],
      load_timeout=max(tp_config.load_timeout, _LOAD_TIMEOUT_SECS))
  try:
    return TraceProcessor(config=config)
  except TraceProcessorException as e:
    # Keep the shell's own error rather than the generic startup message. It
    # is the last line of stderr; debug builds log other lines before it.
    match = re.search(r"stderr: (.*?)\nIf this is", str(e), re.DOTALL)
    lines = match.group(1).strip().splitlines() if match else []
    raise TraceProcessorException(lines[-1] if lines else str(e)) from None


def close_quietly(tp: TraceProcessor) -> None:
  try:
    tp.close()
  except Exception:  # pylint: disable=broad-except
    logger.debug("Error closing TraceProcessor", exc_info=True)


def _close_all(tps: Iterable[TraceProcessor]) -> None:
  # Each close waits for its shell to exit; do them together.
  with ThreadPoolExecutor(max_workers=32) as closer:
    closer.map(close_quietly, tps)


@dc.dataclass(frozen=True)
class PoolQuery:
  """A query as the pool sees it; see TraceProcessorPool.start_query."""
  sql: str = ""
  needs_raw: bool = False
  epoch: int = 0
  query_id: int = 0


@dc.dataclass
class _Instance:
  from_snapshot: bool
  epoch: int
  # Length of the session log this instance is up to date with.
  applied: int = 0
  # Queries this instance ran itself, so are not replayed on it.
  ran: Set[int] = dc.field(default_factory=set)


class TraceProcessorPool:
  """Hands out TraceProcessor instances. Every acquire must be released.

  Released instances are kept loaded, up to `max_size` (0 keeps none), and the
  least recently used are closed. Only idle instances are in the pool, so an
  instance is never shared. Queries run loaded traces first (see is_loaded),
  so a query over more traces than `max_size` reuses the loaded ones instead
  of evicting them all.

  Snapshots: with a SnapshotStore, a trace loads from its snapshot if it has
  one. Otherwise it is parsed, and release() schedules a snapshot, which a
  separate shell makes once no query is running (see start_query).

  Fidelity: snapshots rewrite metadata, stats and trace_file, so a query using
  them (NEEDS_RAW_TRACE) gets an instance parsed from the raw trace.

  Session: a query that turns out to be stateful (it ran statements without
  output such as CREATE or INCLUDE, see QueryResult.is_stateful) is added to a
  session log with add_to_session(). Each instance remembers how much of the
  log it has run and runs the rest when acquired, skipping queries it ran
  itself, so the session survives eviction and reloads. A query that failed
  everywhere never returned a result, so is never added. reset_session()
  clears the log and closes idle instances.
  """

  def __init__(self,
               tp_config: Optional[TraceProcessorConfig] = None,
               max_size: int = 0,
               snapshots: Optional[SnapshotStore] = None):
    self.tp_config = tp_config or TraceProcessorConfig()
    self.max_size = max(0, max_size)
    self.snapshots = snapshots
    self._lock = threading.Lock()
    # Every open instance, idle or in use.
    self._instances: "weakref.WeakKeyDictionary[TraceProcessor, _Instance]" = (
        weakref.WeakKeyDictionary())
    self._idle: "collections.OrderedDict[str, TraceProcessor]" = (
        collections.OrderedDict())
    self._session: List[PoolQuery] = []
    self._epoch = 0
    self._query_ids = itertools.count(1)
    self._closed = False

  def start_query(self, sql: str) -> PoolQuery:
    """Registers a query that is about to run on many traces.

    Holds back snapshots until the matching end_query().
    """
    if self.snapshots:
      self.snapshots.pause()
    with self._lock:
      return PoolQuery(sql, bool(NEEDS_RAW_TRACE.search(sql)), self._epoch,
                       next(self._query_ids))

  def end_query(self) -> None:
    if self.snapshots:
      self.snapshots.resume()

  def add_to_session(self, query: PoolQuery) -> None:
    """Replays the query on instances loaded later. Idempotent."""
    with self._lock:
      if query.epoch == self._epoch and query not in self._session:
        self._session.append(query)

  def reset_session(self) -> None:
    with self._lock:
      self._session.clear()
      self._epoch += 1
      idle = list(self._idle.values())
      self._idle.clear()
    _close_all(idle)

  def acquire(
      self,
      trace_path: str,
      cancel_event: Optional[threading.Event] = None,
      query: PoolQuery = PoolQuery()
  ) -> TraceProcessor:
    tp = self._take_idle(trace_path, query.needs_raw)
    if tp is None:
      tp = self._load(trace_path, query.needs_raw, cancel_event)
    with self._lock:
      inst = self._instances[tp]
      inst.ran.add(query.query_id)
      todo = [
          q.sql
          for q in self._session[inst.applied:]
          if q.query_id not in inst.ran
      ]
      inst.applied = len(self._session)
    try:
      for sql in todo:
        try:
          query_result.query(tp, sql)
        except TraceProcessorException as e:
          # It failed on this trace when it first ran too.
          logger.debug("Replaying session SQL on %s failed: %s", trace_path, e)
    except BaseException:
      close_quietly(tp)
      raise
    return tp

  def release(self,
              trace_path: str,
              tp: TraceProcessor,
              discard: bool = False) -> None:
    with self._lock:
      inst = self._instances.get(tp)
      # Instances from before reset_session() have the old session's state.
      keep = (not discard and inst is not None and inst.epoch == self._epoch and
              not self._closed and self.max_size > 0 and
              trace_path not in self._idle)
      to_close = [] if keep else [tp]
      if keep:
        self._idle[trace_path] = tp
        while len(self._idle) > self.max_size:
          to_close.append(self._idle.popitem(last=False)[1])
    if self.snapshots and inst and not inst.from_snapshot and not discard:
      self.snapshots.schedule(trace_path, self.shell_flags())
    for t in to_close:
      close_quietly(t)

  def is_loaded(self, trace_path: str) -> bool:
    """Whether acquiring the trace would skip loading it."""
    with self._lock:
      return trace_path in self._idle

  def loaded_from_snapshot(self, tp: TraceProcessor) -> bool:
    with self._lock:
      return self._instances[tp].from_snapshot

  def shell_flags(self) -> List[str]:
    """Flags of the shells this pool starts, which snapshots must match."""
    cfg = self.tp_config
    return ([] if cfg.ingest_ftrace_in_raw else ["--no-ftrace-raw"]) + (
        ["--dev"] if cfg.enable_dev_features else []) + list(cfg.extra_flags or
                                                             [])

  def close(self) -> None:
    """Closes every instance, including those in use."""
    with self._lock:
      self._closed = True
      self._idle.clear()
      instances = list(self._instances.keys())
    _close_all(instances)

  def _take_idle(self, trace_path: str, raw: bool) -> Optional[TraceProcessor]:
    with self._lock:
      tp = self._idle.pop(trace_path, None)
      if tp is None:
        return None
      # A shell that died while idle, or a snapshot when the raw trace is
      # needed, is replaced.
      if tp.subprocess.poll() is None and not (
          raw and self._instances[tp].from_snapshot):
        return tp
    close_quietly(tp)
    return None

  def _load(self, trace_path: str, raw: bool,
            cancel_event: Optional[threading.Event]) -> TraceProcessor:
    snapshot = None
    if self.snapshots and not raw:
      snapshot = self.snapshots.find(trace_path)
    if snapshot:
      try:
        tp = load_trace(snapshot, self.tp_config, cancel_event)
        return self._track(tp, from_snapshot=True)
      except TraceProcessorException as e:
        if self._closed or (cancel_event and cancel_event.is_set()):
          raise
        logger.warning("Deleting unloadable snapshot %s: %s", snapshot, e)
        self.snapshots.discard(snapshot)
    tp = load_trace(trace_path, self.tp_config, cancel_event)
    return self._track(tp, from_snapshot=False)

  def _track(self, tp: TraceProcessor, from_snapshot: bool) -> TraceProcessor:
    with self._lock:
      if not self._closed:
        self._instances[tp] = _Instance(from_snapshot, self._epoch)
        return tp
    close_quietly(tp)
    raise TraceProcessorException("Trace processor pool is closed")
