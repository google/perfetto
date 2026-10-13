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
"""Starts a trace_processor_shell per trace.

The server talks to the shells only over HTTP (the DuckDB extension sends
the queries), so a shell here is just a process and its address.
"""

import dataclasses as dc
import logging
import socket
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, List, Optional, Set

from perfetto.trace_processor.api import (
    PLATFORM_DELEGATE,
    TraceProcessor,
    TraceProcessorConfig,
    TraceProcessorException,
)
from perfetto.trace_processor.process_tree import (
    create_kill_on_close_job,
    terminate_process_tree,
)

logger = logging.getLogger(__name__)

# Large traces can take minutes to parse.
_LOAD_TIMEOUT_SECS = 600
_HOST = "127.0.0.1"


def load_trace(
    trace_path: str,
    tp_config: TraceProcessorConfig,
    cancel_event: Optional[threading.Event] = None,
) -> TraceProcessor:
  """Starts a TraceProcessor with the trace loaded."""
  config = dc.replace(
      tp_config,
      cancel_event=cancel_event,
      extra_flags=list(tp_config.extra_flags or []) + [trace_path],
      load_timeout=max(tp_config.load_timeout, _LOAD_TIMEOUT_SECS))
  return TraceProcessor(config=config)


def close_quietly(tp: Any) -> None:
  try:
    tp.close()
  except Exception:  # pylint: disable=broad-except
    logger.debug("Error closing TraceProcessor", exc_info=True)


class ShellStartError(TraceProcessorException):
  """A shell failed to start or load its trace."""


class ShellCancelled(ShellStartError):
  """Startup was cancelled."""


class Shell:
  """A running trace_processor_shell serving one trace over HTTP."""

  def __init__(self, proc: subprocess.Popen, job_handle, port: int, on_close):
    self.proc = proc
    self._job_handle = job_handle
    self.host = _HOST
    self.port = port
    self._on_close = on_close
    self._lock = threading.Lock()
    self._closed = False

  @property
  def closed(self) -> bool:
    return self._closed

  @property
  def subprocess(self) -> Optional[subprocess.Popen]:
    return None if self._closed else self.proc

  def close(self) -> None:
    """Kills the shell; safe to call more than once, from any thread."""
    with self._lock:
      if self._closed:
        return
      self._closed = True
    terminate_process_tree(self.proc, self._job_handle)
    self._on_close(self)


class TraceProcessorPool:
  """Starts and stops a trace_processor_shell for each trace of a query."""

  def __init__(self, tp_config: Optional[TraceProcessorConfig] = None):
    self.tp_config = tp_config or TraceProcessorConfig()
    self._lock = threading.Lock()
    self._shells: Set[Shell] = set()
    # Ports of live shells. A shell binds its port only once its trace is
    # loaded, so the OS can hand the same free port to another shell in
    # between; never reuse a port a live shell owns.
    self._ports: Set[int] = set()
    self._closed = False
    self._bin: Optional[str] = None

  def acquire(
      self,
      trace_path: str,
      cancel_event: Optional[threading.Event] = None,
  ) -> Shell:
    return self._start(trace_path, cancel_event)

  def release(self, shell: Shell) -> None:
    shell.close()

  def shell_flags(self) -> List[str]:
    """Flags of the shells this pool starts."""
    cfg = self.tp_config
    return ([] if cfg.ingest_ftrace_in_raw else ["--no-ftrace-raw"]) + (
        ["--dev"] if cfg.enable_dev_features else []) + list(cfg.extra_flags or
                                                             [])

  def close(self) -> None:
    """Kills every live shell."""
    with self._lock:
      self._closed = True
      shells = list(self._shells)
    for shell in shells:
      shell.close()

  def _shell_path(self) -> str:
    with self._lock:
      if self._bin is None:
        self._bin = PLATFORM_DELEGATE().get_shell_path(
            bin_path=self.tp_config.bin_path,
            fetch_latest=self.tp_config.fetch_latest_trace_processor)
      return self._bin

  def _reserve_port(self) -> int:
    with self._lock:
      while True:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
          s.bind((_HOST, 0))
          port = s.getsockname()[1]
        if port not in self._ports:
          self._ports.add(port)
          return port

  def _forget(self, shell: Shell) -> None:
    with self._lock:
      self._shells.discard(shell)
      self._ports.discard(shell.port)

  def _start(self, path: str, cancel_event: Optional[threading.Event]) -> Shell:
    if self._closed:
      raise ShellStartError("Trace processor pool is closed")
    port = self._reserve_port()
    args = [self._shell_path()]
    if sys.platform == "win32" and not args[0].endswith(".exe"):
      args = [sys.executable] + args
    args += ["-D", "--http-port", str(port)] + self.shell_flags() + [path]
    stderr = tempfile.TemporaryFile()
    try:
      proc = subprocess.Popen(
          args,
          stdin=subprocess.DEVNULL,
          stdout=subprocess.DEVNULL,
          stderr=stderr,
          start_new_session=sys.platform != "win32",
          creationflags=(subprocess.CREATE_NEW_PROCESS_GROUP
                         if sys.platform == "win32" else 0))
    except OSError as e:
      stderr.close()
      with self._lock:
        self._ports.discard(port)
      raise ShellStartError(f"Could not start trace processor: {e}") from e
    shell = Shell(proc, create_kill_on_close_job(proc), port, self._forget)
    with self._lock:
      self._shells.add(shell)
      closed = self._closed
    try:
      if closed:
        raise ShellStartError("Trace processor pool is closed")
      self._wait_ready(shell, cancel_event, stderr)
      return shell
    except BaseException:
      shell.close()
      raise
    finally:
      stderr.close()

  def _wait_ready(self, shell: Shell, cancel_event: Optional[threading.Event],
                  stderr) -> None:
    """Waits until the shell accepts connections, i.e. its trace is loaded.

    Polls with a local TCP connect, which costs microseconds, backing off to
    100 ms for long parses.
    """
    deadline = time.monotonic() + max(self.tp_config.load_timeout,
                                      _LOAD_TIMEOUT_SECS)
    delay = 0.005
    while True:
      if shell.proc.poll() is not None:
        stderr.seek(0)
        lines = stderr.read().decode("utf-8", "replace").strip().splitlines()
        raise ShellStartError(
            lines[-1] if lines else f"trace processor exited with "
            f"{shell.proc.returncode}")
      if shell.closed:
        raise ShellCancelled("Trace processor was stopped")
      if cancel_event is not None and cancel_event.is_set():
        raise ShellCancelled("Trace processor startup cancelled")
      try:
        with socket.create_connection((_HOST, shell.port), timeout=1):
          return
      except OSError:
        pass
      if time.monotonic() > deadline:
        raise ShellStartError("Timed out loading the trace")
      time.sleep(delay)
      delay = min(delay * 1.5, 0.1)
