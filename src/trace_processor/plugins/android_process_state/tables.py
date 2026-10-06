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
from src.trace_processor.tables.metadata_tables import THREAD_TABLE

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
        C('utid', CppOptional(CppTableId(THREAD_TABLE))),
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
                'Process group enum name or value.',
            'reason':
                'Reason for state change (if from track event).',
            'seq_id':
                'OomAdjuster pass sequence id (if from track event).',
            'utid':
                'Emitting thread UniqueTid (if from track event).',
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

SNAPSHOT_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateSnapshotTable',
    sql_name='__intrinsic_android_process_state_snapshot',
    columns=[
        C('ts',
          CppInt64(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('seq_id', CppOptional(CppInt64())),
        C('reason', CppOptional(CppString())),
        C('is_full_update', CppOptional(CppInt32())),
        C('top_pid', CppOptional(CppInt32())),
        C('top_proc_state', CppOptional(CppString())),
        C('target_pids', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='A point-in-time process / service / provider importance graph snapshot.',
        group='Android',
        columns={
            'ts':
                'Start timestamp of the oom-adj pass or snapshot capture.',
            'seq_id':
                'OOM Adjuster sequence ID (mAdjSeq) for this pass.',
            'reason':
                'OomChangeReasonEnum name of the oom-adj pass.',
            'is_full_update':
                '1 if full update, 0 if partial update.',
            'top_pid':
                'PID of the top process at the start of the pass.',
            'top_proc_state':
                'Process state of the top process at the start of the pass.',
            'target_pids':
                'Comma-separated target PIDs for a partial update.',
        },
    ),
)

GRAPH_PROCESS_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateProcessTable',
    sql_name='__intrinsic_android_process_state_process',
    columns=[
        C('snapshot_id', CppUint32()),
        C('pid', CppInt32()),
        C('uid', CppInt32()),
        C('name', CppOptional(CppString())),
        C('oom_score', CppOptional(CppInt32())),
        C('proc_state', CppOptional(CppString())),
        C('capabilities', CppOptional(CppString())),
        C('persistent', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='A process present in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id':
                'The snapshot row id.',
            'pid':
                'Process id.',
            'uid':
                'Process uid.',
            'name':
                'Process name.',
            'oom_score':
                'oom_adj score (lower = more important).',
            'proc_state':
                'ProcessStateEnum name.',
            'capabilities':
                'ProcessCapabilityEnum names granted, " | "-joined.',
            'persistent':
                '1 if a persistent process.',
        },
    ),
)

SERVICE_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateServiceTable',
    sql_name='__intrinsic_android_process_state_service',
    columns=[
        C('snapshot_id', CppUint32()),
        C('svc_id', CppInt32()),
        C('owning_pid', CppOptional(CppInt32())),
        C('uid', CppOptional(CppInt32())),
        C('name', CppOptional(CppString())),
        C('is_foreground', CppOptional(CppInt32())),
        C('foreground_service_type', CppOptional(CppInt32())),
        C('start_requested', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='A service present in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id':
                'The snapshot row id.',
            'svc_id':
                'Service instance id.',
            'owning_pid':
                'Pid of the process hosting the service.',
            'uid':
                'Uid of the service.',
            'name':
                'Short service / component name.',
            'is_foreground':
                '1 if the service is currently a foreground service.',
            'foreground_service_type':
                'Foreground service type mask.',
            'start_requested':
                '1 if startService was requested on this service.',
        },
    ),
)

SERVICE_BINDING_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateServiceBindingTable',
    sql_name='__intrinsic_android_process_state_service_binding',
    columns=[
        C('snapshot_id', CppUint32()),
        C('bind_id', CppInt32()),
        C('client_pid', CppInt32()),
        C('client_uid', CppOptional(CppInt32())),
        C('service_id', CppInt32()),
        C('intent_bind_id', CppOptional(CppInt32())),
        C('foreground', CppOptional(CppInt32())),
        C('flags', CppOptional(CppString())),
        C('intent_action', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='A client->service binding in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id': 'The snapshot row id.',
            'bind_id': 'ConnectionRecord bind_id.',
            'client_pid': 'Pid of the binding client.',
            'client_uid': 'Uid of the binding client.',
            'service_id': 'The bound service svc_id.',
            'intent_bind_id': 'IntentBindRecord id.',
            'foreground': '1 if bound with BIND_FOREGROUND_SERVICE.',
            'flags': 'Human-readable bind flags.',
            'intent_action': 'Intent action of the binding.',
        },
    ),
)

PROVIDER_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateProviderTable',
    sql_name='__intrinsic_android_process_state_provider',
    columns=[
        C('snapshot_id', CppUint32()),
        C('provider_id', CppInt32()),
        C('owning_pid', CppOptional(CppInt32())),
        C('uid', CppOptional(CppInt32())),
        C('authority', CppOptional(CppString())),
        C('component_name', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='A content provider present in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id': 'The snapshot row id.',
            'provider_id': 'ContentProviderRecord provider_id.',
            'owning_pid': 'Pid of the process hosting the provider.',
            'uid': 'Uid of the provider.',
            'authority': 'Content provider authority.',
            'component_name': 'Content provider component name.',
        },
    ),
)

PROVIDER_BINDING_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateProviderBindingTable',
    sql_name='__intrinsic_android_process_state_provider_binding',
    columns=[
        C('snapshot_id', CppUint32()),
        C('bind_id', CppInt32()),
        C('client_pid', CppInt32()),
        C('client_uid', CppOptional(CppInt32())),
        C('provider_id', CppInt32()),
        C('stable', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='A client->provider binding in a process-state snapshot.',
        group='Android',
        columns={
            'snapshot_id': 'The snapshot row id.',
            'bind_id': 'ContentProviderConnection bind_id.',
            'client_pid': 'Pid of the binding client.',
            'client_uid': 'Uid of the binding client.',
            'provider_id': 'The referenced provider_id.',
            'stable': '1 if a stable provider connection.',
        },
    ),
)

TRIGGER_EVENT_TABLE = Table(
    python_module=__file__,
    class_name='AndroidProcessStateTriggerEventTable',
    sql_name='__intrinsic_android_process_state_trigger_event',
    columns=[
        C('ts', CppInt64()),
        C('seq_id', CppOptional(CppInt64())),
        C('kind', CppString()),
        C('action', CppOptional(CppString())),
        C('component_name', CppOptional(CppString())),
        C('target_pid', CppOptional(CppInt32())),
        C('target_uid', CppOptional(CppInt32())),
        C('caller_pid', CppOptional(CppInt32())),
        C('caller_uid', CppOptional(CppInt32())),
        C('detail', CppOptional(CppString())),
        C('service_id', CppOptional(CppInt32())),
        C('bind_id', CppOptional(CppInt32())),
        C('intent_bind_id', CppOptional(CppInt32())),
        C('provider_id', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='Causal events (service, provider, broadcast, activity, PSC trigger, process lifecycle) correlated with OOM Adjuster passes via seq_id.',
        group='Android',
        columns={
            'ts':
                'Event timestamp.',
            'seq_id':
                'Correlated OOM Adjuster sequence ID.',
            'kind':
                'Event category (service, fgs, provider, broadcast, activity, psc_trigger, process_start, freezer).',
            'action':
                'Specific event action or transition (e.g., BIND, UNBIND, RESUMED, broadcast_dispatch_started, setTopProcess).',
            'component_name':
                'Component name, broadcast action, or authority.',
            'target_pid':
                'Target/hosting process PID.',
            'target_uid':
                'Target/hosting process UID.',
            'caller_pid':
                'Caller/client process PID.',
            'caller_uid':
                'Caller/client process UID.',
            'detail':
                'Additional details (flags, prev->cur state, hosting type, etc.).',
            'service_id':
                'ServiceRecord id (joins android_process_state_service).',
            'bind_id':
                'ConnectionRecord / ContentProviderConnection id (joins the '
                'service or provider binding tables).',
            'intent_bind_id':
                'IntentBindRecord id shared by all connections of one Intent.',
            'provider_id':
                'ContentProviderRecord id (joins android_process_state_provider).',
        },
    ),
)

ALL_TABLES = [
    ANDROID_FREEZER_STATE_TABLE,
    ANDROID_PROCESS_STATE_TABLE,
    GRAPH_PROCESS_TABLE,
    PROVIDER_BINDING_TABLE,
    PROVIDER_TABLE,
    SERVICE_BINDING_TABLE,
    SERVICE_TABLE,
    SNAPSHOT_TABLE,
    TRIGGER_EVENT_TABLE,
]
