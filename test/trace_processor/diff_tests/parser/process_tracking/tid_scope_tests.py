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

from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

_INIT_PROCESS = 'processes { pid: 1 ppid: 0 cmdline: "/sbin/init" }'


def _sched(tid, comm):
  return '''
packet {
  ftrace_events {
    cpu: 0
    event {
      timestamp: 100
      pid: 0
      sched_switch {
        prev_comm: "swapper/0" prev_pid: 0 prev_prio: 120 prev_state: 0
        next_comm: "%s" next_pid: %d next_prio: 120
      }
    }
    event {
      timestamp: 200
      pid: %d
      sched_switch {
        prev_comm: "%s" prev_pid: %d prev_prio: 120 prev_state: 1
        next_comm: "swapper/0" next_pid: 0 next_prio: 120
      }
    }
  }
}
''' % (comm, tid, tid, comm, tid)


def _process_tree(*entries):
  return '''
packet {
  timestamp: 1
  process_tree {
    %s
  }
}
''' % '\n    '.join(entries)


def _renderer_thread(tid, chrome_thread=''):
  return '''
packet {
  trusted_packet_sequence_id: 1
  timestamp: 10
  incremental_state_cleared: true
  track_descriptor {
    uuid: 42
    thread { pid: 1000 tid: %d thread_name: "CrRendererMain" }
    %s
  }
}
packet {
  trusted_packet_sequence_id: 1
  timestamp: 20
  track_event { track_uuid: 42 type: TYPE_SLICE_BEGIN name: "RendererWork" }
}
packet {
  trusted_packet_sequence_id: 1
  timestamp: 30
  track_event { track_uuid: 42 type: TYPE_SLICE_END }
}
''' % (tid, chrome_thread)


_SANDBOXED = 'chrome_thread { is_sandboxed_tid: true }'

# Which thread and process every sched slice and thread slice is attributed to.
_ATTRIBUTION_QUERY = '''
SELECT 'sched' AS src, t.tid, t.name AS thread, p.pid, p.name AS process
FROM sched s
JOIN thread t USING (utid)
LEFT JOIN process p USING (upid)
WHERE t.tid != 0
UNION ALL
SELECT 'slice', t.tid, t.name, p.pid, p.name
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t USING (utid)
LEFT JOIN process p USING (upid);
'''


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

  # A sandboxed process's tid 1 is not init's: init's sched slices must stay on
  # init.
  def test_sandboxed_tid_does_not_steal_kernel_tid(self):
    return DiffTestBlueprint(
        trace=TextProto(
            _process_tree(
                _INIT_PROCESS,
                'processes { pid: 1000 ppid: 1 cmdline: "renderer" }') +
            _renderer_thread(1, _SANDBOXED) + _sched(1, 'init')),
        query=_ATTRIBUTION_QUERY,
        out=Csv('''
        "src","tid","thread","pid","process"
        "sched",1,"init",1,"/sbin/init"
        "slice",1,"CrRendererMain",1000,"renderer"
        '''))

  # A StackSample for the same (pid, tid) as a sandboxed track_event thread
  # must land on that same thread.
  def test_sandboxed_tid_shared_with_stack_sample(self):
    return DiffTestBlueprint(
        trace=TextProto(
            _process_tree('processes { pid: 1000 ppid: 1 cmdline: "renderer" }')
            + '''
        packet {
          trusted_packet_sequence_id: 2
          timestamp: 5
          incremental_state_cleared: true
          trace_packet_defaults {
            stack_sample_defaults {
              source: "chrome"
              primary_descriptor { name: "samples" unit: UNIT_COUNT }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          timestamp: 6
          stack_sample {
            task_context { pid: 1000 tid: 1 }
            callstack { frames { function_name: "main" } }
            primary_weight: 1
          }
        }
        ''' + _renderer_thread(1, _SANDBOXED)),
        query='''
        SELECT
          (SELECT COUNT(*) FROM thread t JOIN process p USING (upid)
           WHERE p.pid = 1000 AND t.tid = 1) AS threads,
          (SELECT value FROM stats WHERE name = 'namespaced_tid_unresolved')
            AS unresolved;
        ''',
        out=Csv('''
        "threads","unresolved"
        1,1
        '''))

  # ProcessTree says the process is pid-namespaced but has no entry for this
  # thread: its tid cannot be a kernel tid, so it must not take over
  # kthreadd's.
  def test_unresolved_namespaced_tid_does_not_steal_kernel_tid(self):
    return DiffTestBlueprint(
        trace=TextProto(
            _process_tree(
                _INIT_PROCESS,
                'processes { pid: 2 ppid: 0 cmdline: "kthreadd" }',
                'processes { pid: 1000 ppid: 1 cmdline: "renderer" '
                'nspid: 1000 nspid: 1 }') + _renderer_thread(2) +
            _sched(2, 'kthreadd')),
        query=_ATTRIBUTION_QUERY,
        out=Csv('''
        "src","tid","thread","pid","process"
        "sched",2,"kthreadd",2,"kthreadd"
        "slice",2,"CrRendererMain",1000,"renderer"
        '''))

  # With the mapping in ProcessTree, the namespaced tid resolves to its kernel
  # tid and joins with the kernel's view of the thread.
  def test_namespaced_tid_resolved_via_process_tree(self):
    return DiffTestBlueprint(
        trace=TextProto(
            _process_tree(
                _INIT_PROCESS, 'processes { pid: 1000 ppid: 1 '
                'cmdline: "renderer" nspid: 1000 nspid: 1 }',
                'threads { tid: 1005 tgid: 1000 nstid: 1005 nstid: 2 }') +
            _renderer_thread(2) + _sched(1005, 'CrRendererMain')),
        query=_ATTRIBUTION_QUERY,
        out=Csv('''
        "src","tid","thread","pid","process"
        "sched",1005,"CrRendererMain",1000,"renderer"
        "slice",1005,"CrRendererMain",1000,"renderer"
        '''))

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
