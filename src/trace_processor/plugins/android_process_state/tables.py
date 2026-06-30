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

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppAccessDuration
from python.generators.trace_processor_table.public import CppInt32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

# A per-process process-state timeline from the AndroidProcessStateChangedEvent
# track-event stream and the trace-stop AndroidProcessState dump. One row per
# observed change (carrying the new state), plus one backfill row (from the dump,
# stamped at trace start) for each process that never emitted a change -- so its
# state is known even when the ring buffer wrapped. Enum-valued fields are stored
# as their resolved names (via the generated <Enum>_Name() helpers).
CHANGE_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateChangeTable',
    sql_name='__intrinsic_android_process_state_change',
    columns=[
        C('upid', CppUint32()),
        C('pid', CppInt32()),
        C('uid', CppOptional(CppInt32())),
        C('ts',
          CppOptional(CppInt64()),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('proc_state', CppOptional(CppString())),
        C('oom_score', CppOptional(CppInt32())),
        C('capability_flags', CppOptional(CppInt32())),
        C('reason', CppOptional(CppString())),
        C('seq_id', CppOptional(CppInt64())),
        C('is_initial', CppUint32()),
    ],
    tabledoc=TableDoc(
        doc='''
          A per-process process-state timeline. Each observed change is one row.
          Every process also gets one initial-state row (is_initial = 1, ts NULL)
          holding its state at the start of the trace: from the earliest delta's
          prev_* for a process that changed, or from the trace-stop dump for one
          that did not.
        ''',
        group='Android',
        columns={
            'upid': 'The process (joins __intrinsic_process.id).',
            'pid': 'Process id.',
            'uid': 'Process uid.',
            'ts': 'Change timestamp; NULL for an initial-state row.',
            'proc_state': 'ProcessStateEnum name after the change (e.g. '
                          '"PROCESS_STATE_TOP").',
            'oom_score': 'oom_adj score after the change (lower = more '
                         'important).',
            'capability_flags':
                'Granted capabilities after the change; bitmask '
                'of ProcessCapabilityEnum.',
            'reason':
                'OomChangeReasonEnum name of the oom-adj pass that caused '
                'the change; NULL for an initial-state row.',
            'seq_id':
                'oom-adj pass sequence id the change belongs to; NULL for '
                'an initial-state row.',
            'is_initial': '1 if this is the synthesized initial-state row '
                          '(ts NULL); 0 for an observed change.',
        }))

# Keep this list sorted.
ALL_TABLES = [
    CHANGE_TABLE,
]
