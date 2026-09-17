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
        C('reason', CppOptional(CppString())),
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
            'reason':
                'Reason for state change (if from track event).',
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

ALL_TABLES = [
    ANDROID_PROCESS_STATE_TABLE,
    ANDROID_FREEZER_STATE_TABLE,
]
