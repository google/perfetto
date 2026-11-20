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
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppAccessDuration
from python.generators.trace_processor_table.public import CppInt32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc
from src.trace_processor.tables.metadata_tables import PROCESS_TABLE

ANDROID_PROCESS_STATE_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateTable',
    sql_name='__intrinsic_android_process_state',
    columns=[
        C('ts',
          CppOptional(CppInt64()),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('upid', CppTableId(PROCESS_TABLE), cpp_access=CppAccess.READ),
        C('proc_state', CppOptional(CppString())),
        C('oom_score', CppOptional(CppInt32())),
        C('capability_flags', CppOptional(CppInt32())),
        C('process_group', CppOptional(CppString())),
        C('reason', CppOptional(CppString())),
        C('seq_id', CppOptional(CppInt64())),
        C('cpu_time_reasons', CppOptional(CppInt32())),
        C('implicit_cpu_time_reasons', CppOptional(CppInt32())),
        C('freeze_policy_flags', CppOptional(CppInt32())),
        C('arg_set_id',
          CppOptional(CppUint32()),
          cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE),
        C('is_initial', CppUint32(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc='Per-process state events from snapshots and track events.',
        group='Android',
        columns={
            'ts':
                'Timestamp of event; NULL for initial backfill row.',
            'upid':
                'Unique process ID.',
            'proc_state':
                'Process state enum name or value.',
            'oom_score':
                'OOM score.',
            'capability_flags':
                'Capability flags.',
            'process_group':
                'Process group enum name or value, as seen by OomAdjuster '
                '(the scheduling group it computed for the process). See '
                '__intrinsic_android_process_group for the groups '
                'requested through Process.setProcessGroup.',
            'reason':
                'Reason for state change (if from track event).',
            'seq_id':
                'OOM adjuster sequence ID (if from track event).',
            'cpu_time_reasons':
                'Bitmask of reasons for granting PROCESS_CAPABILITY_CPU_TIME '
                '(if from track event).',
            'implicit_cpu_time_reasons':
                'Bitmask of reasons for granting '
                'PROCESS_CAPABILITY_IMPLICIT_CPU_TIME (if from track event).',
            'freeze_policy_flags':
                'Bitmask of boolean freeze policy states '
                '(if from track event).',
            'arg_set_id':
                ColumnDoc(
                    'Argument set holding decoded capability_flags[i], '
                    'cpu_time_reasons[i], implicit_cpu_time_reasons[i], and '
                    'freeze_policy_flags[i] entries.',
                    joinable='args.arg_set_id'),
            'is_initial':
                '1 for synthesized initial state row, 0 for change event.',
        },
    ),
)

ANDROID_FREEZER_STATE_TABLE = Table(
    python_module=__file__,
    class_name='AndroidFreezerStateTable',
    sql_name='__intrinsic_android_freezer_state',
    columns=[
        C('ts',
          CppOptional(CppInt64()),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('upid', CppTableId(PROCESS_TABLE), cpp_access=CppAccess.READ),
        C('unfrozen_dur_ms', CppOptional(CppInt64()),
          cpp_access=CppAccess.READ),
        C('frozen_dur_ms', CppOptional(CppInt64()), cpp_access=CppAccess.READ),
        C('unfreeze_reason',
          CppOptional(CppString()),
          cpp_access=CppAccess.READ),
        C('is_initial', CppUint32(), cpp_access=CppAccess.READ),
    ],
    tabledoc=TableDoc(
        doc='Process freezer events from snapshots and track events.',
        group='Android',
        columns={
            'ts':
                'Timestamp of freezer event; NULL for initial backfill row.',
            'upid':
                'Unique process ID.',
            'unfrozen_dur_ms':
                'Time since last unfrozen in milliseconds.',
            'frozen_dur_ms':
                'Time since last frozen in milliseconds.',
            'unfreeze_reason':
                'Reason for unfreezing.',
            'is_initial':
                '1 for synthesized initial state row, 0 for track event.',
        },
    ),
)

ANDROID_PROCESS_GROUP_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessGroupTable',
    sql_name='__intrinsic_android_process_group',
    columns=[
        C('ts', CppInt64()),
        C('upid', CppTableId(PROCESS_TABLE)),
        C('uid', CppOptional(CppInt32())),
        C('process_group', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          Process groups requested through Process.setProcessGroup, one row
          per call, from AndroidProcessGroupChangedEvent track events.
          The calling process emits the event (on its own track) before the
          native call, which can fail (e.g. when the target process is gone),
          and also when the group does not change, so rows are requests
          rather than deduplicated changes of the process's actual group.
          Unlike __intrinsic_android_process_state.process_group (OomAdjuster's
          view), this reflects every caller of Process.setProcessGroup. There
          are no initial rows: the event carries no previous group, so the
          group before a process's first event is unknown.
        ''',
        group='Android',
        columns={
            'ts': 'Timestamp of the Process.setProcessGroup call.',
            'upid': 'Unique process ID.',
            'uid':
                'Uid of the target process, if the caller passed it '
                '(Process.setProcessGroup(uid, pid, group)); NULL otherwise.',
            'process_group':
                'Requested process group enum name (e.g. '
                'PROCESS_GROUP_TOP_APP), or its value if unknown; NULL if the '
                'event has no group.',
        },
    ),
)

ALL_TABLES = [
    ANDROID_PROCESS_STATE_TABLE,
    ANDROID_FREEZER_STATE_TABLE,
    ANDROID_PROCESS_GROUP_TABLE,
]
