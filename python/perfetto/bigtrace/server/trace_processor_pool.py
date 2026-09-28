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
"""Strategies for acquiring a TraceProcessor per trace."""

import collections
from concurrent.futures import ThreadPoolExecutor
import dataclasses as dc
import logging
import re
import threading
from typing import Optional

from perfetto.trace_processor.api import (
    TraceProcessor,
    TraceProcessorConfig,
    TraceProcessorException,
)

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
    # Keep the shell's own error rather than the generic startup message.
    match = re.search(r"stderr: (.+)", str(e))
    raise TraceProcessorException(match.group(1) if match else str(e)) from None


def close_quietly(tp: TraceProcessor) -> None:
  try:
    tp.close()
  except Exception:  # pylint: disable=broad-except
    logger.debug("Error closing TraceProcessor", exc_info=True)


class TraceProcessorPool:
  """Hands out TraceProcessor instances. Every acquire must be released."""

  def acquire(self,
              trace_path: str,
              cancel_event: Optional[threading.Event] = None) -> TraceProcessor:
    raise NotImplementedError

  def release(self,
              trace_path: str,
              tp: TraceProcessor,
              discard: bool = False) -> None:
    raise NotImplementedError

  def is_loaded(self, trace_path: str) -> bool:
    """Whether acquiring the trace would skip loading it."""
    return False

  def close(self) -> None:
    pass


class EphemeralTraceProcessorPool(TraceProcessorPool):
  """Loads the trace for every query. Lowest memory use."""

  def __init__(self, tp_config: Optional[TraceProcessorConfig] = None):
    self.tp_config = tp_config or TraceProcessorConfig()

  def acquire(self,
              trace_path: str,
              cancel_event: Optional[threading.Event] = None) -> TraceProcessor:
    return load_trace(trace_path, self.tp_config, cancel_event)

  def release(self,
              trace_path: str,
              tp: TraceProcessor,
              discard: bool = False) -> None:
    close_quietly(tp)


class KeepAliveTraceProcessorPool(TraceProcessorPool):
  """Keeps loaded traces between queries so repeated queries skip parsing.

  Only idle instances live in the pool, so an instance is never shared. At most
  `max_size` idle instances are kept; the least recently used are closed.
  Queries run loaded traces first (see is_loaded), so a query over more traces
  than `max_size` still reuses the loaded ones instead of evicting them all.
  """

  def __init__(self,
               tp_config: Optional[TraceProcessorConfig] = None,
               max_size: int = 50):
    self.tp_config = tp_config or TraceProcessorConfig()
    self.max_size = max(1, max_size)
    self._lock = threading.Lock()
    self._idle: "collections.OrderedDict[str, TraceProcessor]" = (
        collections.OrderedDict())
    self._closed = False

  def acquire(self,
              trace_path: str,
              cancel_event: Optional[threading.Event] = None) -> TraceProcessor:
    with self._lock:
      tp = self._idle.pop(trace_path, None)
    return tp or load_trace(trace_path, self.tp_config, cancel_event)

  def is_loaded(self, trace_path: str) -> bool:
    with self._lock:
      return trace_path in self._idle

  def release(self,
              trace_path: str,
              tp: TraceProcessor,
              discard: bool = False) -> None:
    to_close = []
    with self._lock:
      # A concurrent query may already have returned one for the same trace.
      if discard or self._closed or trace_path in self._idle:
        to_close.append(tp)
      else:
        self._idle[trace_path] = tp
        while len(self._idle) > self.max_size:
          to_close.append(self._idle.popitem(last=False)[1])
    for t in to_close:
      close_quietly(t)

  def close(self) -> None:
    with self._lock:
      self._closed = True
      to_close = list(self._idle.values())
      self._idle.clear()
    # Each close waits for its shell to exit; do them together.
    with ThreadPoolExecutor(max_workers=32) as closer:
      closer.map(close_quietly, to_close)
