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

import json

from python.generators.diff_tests.testing import Csv, Json
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


def _gecko_thread(name, pid, times):
  n = len(times)
  thread = {
      'name': name,
      'tid': 5,
      'stringArray': ['Marker' + name[-1]],
      'frameTable': {
          'func': []
      },
      'funcTable': {
          'name': []
      },
      'stackTable': {
          'prefix': [],
          'frame': []
      },
      'samples': {
          'stack': [],
          'time': []
      },
      'markers': {
          'name': [0] * n,
          'startTime': times,
          'endTime': [t + 1.0 for t in times],
          'phase': [1] * n,
          'category': [0] * n,
          'data': [None] * n,
          'length': n,
      },
  }
  if pid is not None:
    thread['pid'] = pid
  return thread


def _gecko_profile(threads):
  return {
      'meta': {
          'categories': [{
              'name': 'Other',
              'color': 'grey',
              'subcategories': ['Other']
          }]
      },
      'threads': threads,
  }


# The process of the thread holding the slice, and how many processes exist
# (the swapper is always one).
_NO_PID_QUERY = '''
SELECT t.tid, t.upid, (SELECT COUNT(*) FROM process) AS processes
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t USING (utid);
'''


class TidScopes(TestSuite):

  # Two JSON processes with string ids that both name a thread "Main" must
  # each keep their own named thread.
  def test_json_string_tid_per_process(self):
    return DiffTestBlueprint(
        trace=Json(
            json.dumps({
                'traceEvents': [
                    {
                        'pid': 'Browser',
                        'tid': 'Main',
                        'ph': 'X',
                        'ts': 10,
                        'dur': 5,
                        'name': 'BrowserWork'
                    },
                    {
                        'pid': 'Renderer',
                        'tid': 'Main',
                        'ph': 'X',
                        'ts': 20,
                        'dur': 5,
                        'name': 'RendererWork'
                    },
                ]
            })),
        query='''
        SELECT s.name AS slice, t.name AS thread, p.name AS process
        FROM slice s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread t USING (utid)
        JOIN process p USING (upid)
        ORDER BY s.ts;
        ''',
        out=Csv('''
        "slice","thread","process"
        "BrowserWork","Main","Browser"
        "RendererWork","Main","Renderer"
        '''))

  # Gecko: two processes with the same tid must keep their own markers, even
  # when one thread's markers interleave with the other's.
  def test_gecko_same_tid_two_processes(self):
    threads = [
        _gecko_thread('ThreadA', 100, [10.0, 30.0]),
        _gecko_thread('ThreadB', 200, [20.0]),
    ]
    return DiffTestBlueprint(
        trace=Json(json.dumps(_gecko_profile(threads))),
        query='''
        SELECT s.name AS marker, s.ts, t.name AS thread, p.pid
        FROM slice s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread t USING (utid)
        LEFT JOIN process p USING (upid)
        ORDER BY s.ts;
        ''',
        out=Csv('''
        "marker","ts","thread","pid"
        "MarkerA",10000000,"ThreadA",100
        "MarkerB",20000000,"ThreadB",200
        "MarkerA",30000000,"ThreadA",100
        '''))

  # A JSON event without a pid belongs to an unknown process, not pid 0.
  def test_json_thread_without_pid_has_no_process(self):
    return DiffTestBlueprint(
        trace=Json(
            json.dumps({
                'traceEvents': [{
                    'tid': 5,
                    'ph': 'X',
                    'ts': 10,
                    'dur': 5,
                    'name': 'Work'
                }]
            })),
        query=_NO_PID_QUERY,
        out=Csv('''
        "tid","upid","processes"
        5,"[NULL]",1
        '''))

  # Likewise for a Gecko thread without a pid.
  def test_gecko_thread_without_pid_has_no_process(self):
    thread = _gecko_thread('ThreadA', None, [10.0])
    thread['tid'] = 5
    return DiffTestBlueprint(
        trace=Json(json.dumps(_gecko_profile([thread]))),
        query=_NO_PID_QUERY,
        out=Csv('''
        "tid","upid","processes"
        5,"[NULL]",1
        '''))
