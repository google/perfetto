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

# One row per ProcessStateSnapshot packet (a `dumpsys activity` snapshot, or one
# reconstructed from the oom-adj event stream). The process / service / provider
# tables below reference it by snapshot_id (its row id). Enum-valued fields are
# stored as their resolved names (via the generated <Enum>_Name() helpers), so
# the UI needs no enum tables of its own.
SNAPSHOT_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateSnapshotTable',
    sql_name='__intrinsic_android_process_state_snapshot',
    columns=[
        C('ts',
          CppInt64(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('reason', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          A point-in-time process / service / provider importance graph snapshot
          (the data behind `dumpsys activity`).
        ''',
        group='Android',
        columns={
            'ts': 'Start timestamp (first event of the oom-adj pass; the '
                  'dumpsys capture time for a one-shot snapshot). The snapshot '
                  'is current until the next snapshot.',
            'reason':
                'OomChangeReasonEnum name of the oom-adj pass that '
                'produced this snapshot when reconstructed from the event '
                'stream; NULL for a one-shot dumpsys snapshot.',
        }))

# Processes in a snapshot (the nodes of the graph).
PROCESS_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateProcessTable',
    sql_name='__intrinsic_android_process_state_process',
    columns=[
        C('snapshot_id', CppUint32()),
        C('pid', CppInt32()),
        C('uid', CppInt32()),
        C('name', CppString()),
        C('oom_score', CppOptional(CppInt32())),
        C('proc_state', CppOptional(CppString())),
        C('capabilities', CppOptional(CppString())),
        C('persistent', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='A process present in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id': 'The snapshot row id.',
            'pid': 'Process id.',
            'uid': 'Process uid.',
            'name': 'Process name.',
            'oom_score': 'oom_adj score (lower = more important).',
            'proc_state': 'ProcessStateEnum name (e.g. "BOUND_FOREGROUND_'
                          'SERVICE").',
            'capabilities': 'ProcessCapabilityEnum names granted, " | "-joined '
                            '("none" if zero).',
            'persistent': '1 if a persistent process.',
        }))

# Services in a snapshot, referenced by service bindings via svc_id.
SERVICE_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateServiceTable',
    sql_name='__intrinsic_android_process_state_service',
    columns=[
        C('snapshot_id', CppUint32()),
        C('svc_id', CppInt32()),
        C('owning_pid', CppOptional(CppInt32())),
        C('name', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='A service present in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id': 'The snapshot row id.',
            'svc_id': 'Per-snapshot service id, referenced by '
                      '__intrinsic_android_process_state_service_binding.'
                      'service_id.',
            'owning_pid': 'Pid of the process hosting the service.',
            'name': 'Short service / component name.',
        }))

# Client -> service bindings in a snapshot (edges of the graph).
SERVICE_BINDING_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateServiceBindingTable',
    sql_name='__intrinsic_android_process_state_service_binding',
    columns=[
        C('snapshot_id', CppUint32()),
        C('client_pid', CppInt32()),
        C('service_id', CppInt32()),
        C('foreground', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='A client->service binding in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id': 'The snapshot row id.',
            'client_pid': 'Pid of the binding client.',
            'service_id': 'The bound service svc_id (same snapshot).',
            'foreground': '1 if bound with BIND_FOREGROUND_SERVICE.',
        }))

# Content providers in a snapshot, referenced by provider bindings.
PROVIDER_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateProviderTable',
    sql_name='__intrinsic_android_process_state_provider',
    columns=[
        C('snapshot_id', CppUint32()),
        C('provider_id', CppInt32()),
        C('owning_pid', CppOptional(CppInt32())),
        C('authority', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='A content provider present in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id': 'The snapshot row id.',
            'provider_id': 'Per-snapshot provider id, referenced by '
                           '__intrinsic_android_process_state_provider_binding.'
                           'provider_id.',
            'owning_pid': 'Pid of the process hosting the provider.',
            'authority': 'Content provider authority.',
        }))

# Client -> provider bindings in a snapshot (edges of the graph).
PROVIDER_BINDING_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateProviderBindingTable',
    sql_name='__intrinsic_android_process_state_provider_binding',
    columns=[
        C('snapshot_id', CppUint32()),
        C('client_pid', CppInt32()),
        C('provider_id', CppInt32()),
        C('stable', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='A client->provider binding in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id': 'The snapshot row id.',
            'client_pid': 'Pid of the binding client.',
            'provider_id': 'The referenced provider provider_id (same '
                           'snapshot).',
            'stable': '1 if a stable provider connection.',
        }))

# Keep this list sorted.
ALL_TABLES = [
    PROCESS_TABLE,
    PROVIDER_BINDING_TABLE,
    PROVIDER_TABLE,
    SERVICE_BINDING_TABLE,
    SERVICE_TABLE,
    SNAPSHOT_TABLE,
]
