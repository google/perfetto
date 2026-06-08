#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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
"""Cross-platform teardown of a spawned process and all of its descendants.

A spawned trace_processor can have descendants (e.g. when launched via a Python
wrapper script), and killing only the direct child would orphan them. The two
platforms need different machinery:

- POSIX: the caller spawns the child with start_new_session=True so it leads its
  own process group; terminate_process_tree() then reaps the group with
  killpg(SIGKILL).
- Windows: the child is assigned (via create_kill_on_close_job) to a Job Object
  configured to kill all member processes when its last handle is closed;
  terminate_process_tree() calls TerminateJobObject. Closing the handle also
  means the tree is torn down if the owning Python process dies for any reason.
"""

import os
import signal
import subprocess
import sys

if sys.platform == 'win32':
  import ctypes
  from ctypes import wintypes

  _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
  _JobObjectExtendedLimitInformation = 9

  _kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
  _kernel32.CreateJobObjectW.restype = wintypes.HANDLE
  _kernel32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
  _kernel32.SetInformationJobObject.restype = wintypes.BOOL
  _kernel32.SetInformationJobObject.argtypes = [
      wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD
  ]
  _kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
  _kernel32.AssignProcessToJobObject.argtypes = [
      wintypes.HANDLE, wintypes.HANDLE
  ]
  _kernel32.TerminateJobObject.restype = wintypes.BOOL
  _kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
  _kernel32.CloseHandle.restype = wintypes.BOOL
  _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

  class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ('PerProcessUserTimeLimit', wintypes.LARGE_INTEGER),
        ('PerJobUserTimeLimit', wintypes.LARGE_INTEGER),
        ('LimitFlags', wintypes.DWORD),
        ('MinimumWorkingSetSize', ctypes.c_size_t),
        ('MaximumWorkingSetSize', ctypes.c_size_t),
        ('ActiveProcessLimit', wintypes.DWORD),
        ('Affinity', ctypes.c_size_t),
        ('PriorityClass', wintypes.DWORD),
        ('SchedulingClass', wintypes.DWORD),
    ]

  class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        ('ReadOperationCount', ctypes.c_ulonglong),
        ('WriteOperationCount', ctypes.c_ulonglong),
        ('OtherOperationCount', ctypes.c_ulonglong),
        ('ReadTransferCount', ctypes.c_ulonglong),
        ('WriteTransferCount', ctypes.c_ulonglong),
        ('OtherTransferCount', ctypes.c_ulonglong),
    ]

  class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ('BasicLimitInformation', _JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ('IoInfo', _IO_COUNTERS),
        ('ProcessMemoryLimit', ctypes.c_size_t),
        ('JobMemoryLimit', ctypes.c_size_t),
        ('PeakProcessMemoryUsed', ctypes.c_size_t),
        ('PeakJobMemoryUsed', ctypes.c_size_t),
    ]

  def create_kill_on_close_job(proc: subprocess.Popen):
    """Creates a kill-on-close Job Object and assigns |proc| to it.

    Returns the job handle, which must later be passed to
    terminate_process_tree(). Returns None if the OS rejected any step, in which
    case terminate_process_tree() falls back to killing the process directly.
    """
    job = _kernel32.CreateJobObjectW(None, None)
    if not job:
      return None
    info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not _kernel32.SetInformationJobObject(
        job, _JobObjectExtendedLimitInformation, ctypes.byref(info),
        ctypes.sizeof(info)):
      _kernel32.CloseHandle(job)
      return None
    if not _kernel32.AssignProcessToJobObject(job, int(proc._handle)):
      _kernel32.CloseHandle(job)
      return None
    return job
else:

  def create_kill_on_close_job(proc: subprocess.Popen):
    """No-op on POSIX, where the process group is used instead (see module
    docstring). Always returns None."""
    return None


def terminate_process_tree(p: subprocess.Popen, job_handle=None):
  """Forcibly terminates |p| and all of its descendants.

  |job_handle| is the Windows Job Object returned by create_kill_on_close_job
  (None on POSIX or if Job Object setup failed). This never sends a graceful
  signal, so the subsequent wait() returns promptly and cannot hang.
  """
  if sys.platform == 'win32':
    if job_handle is not None:
      _kernel32.TerminateJobObject(job_handle, 1)
      _kernel32.CloseHandle(job_handle)
    else:
      # Job Object setup failed; best-effort kill of the direct child.
      p.kill()
  else:
    try:
      # The child is its own process group leader (start_new_session=True), so
      # its pid is also the process group id. Killing the group reaps any
      # grandchildren too.
      os.killpg(p.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
      # Already gone, or the group no longer exists.
      pass
  p.wait()
