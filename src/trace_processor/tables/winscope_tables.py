# Copyright (C) 2023 The Android Open Source Project
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
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import TableDoc
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import CppString

SURFACE_FLINGER_LAYERS_SNAPSHOT_TABLE = Table(
    python_module=__file__,
    class_name='SurfaceFlingerLayersSnapshotTable',
    sql_name='surfaceflinger_layers_snapshot',
    columns=[
        C('ts', CppInt64()),
        C('arg_set_id', CppUint32()),
    ],
    tabledoc=TableDoc(
        doc='SurfaceFlinger layers snapshot',
        group='Winscope',
        columns={
            'ts': 'Timestamp of the snapshot',
            'arg_set_id': 'Extra args parsed from the proto message',
        }))

SURFACE_FLINGER_LAYER_TABLE = Table(
    python_module=__file__,
    class_name='SurfaceFlingerLayerTable',
    sql_name='surfaceflinger_layer',
    columns=[
        C('snapshot_id', CppTableId(SURFACE_FLINGER_LAYERS_SNAPSHOT_TABLE)),
        C('arg_set_id', CppUint32()),
    ],
    tabledoc=TableDoc(
        doc='SurfaceFlinger layer',
        group='Winscope',
        columns={
            'snapshot_id': 'The snapshot that generated this layer',
            'arg_set_id': 'Extra args parsed from the proto message',
        }))

SURFACE_FLINGER_TRANSACTIONS_TABLE = Table(
    python_module=__file__,
    class_name='SurfaceFlingerTransactionsTable',
    sql_name='surfaceflinger_transactions',
    columns=[
        C('ts', CppInt64()),
        C('arg_set_id', CppUint32()),
    ],
    tabledoc=TableDoc(
        doc='SurfaceFlinger transactions. Each row contains a set of ' +
        'transactions that SurfaceFlinger committed together.',
        group='Winscope',
        columns={
            'ts': 'Timestamp of the transactions commit',
            'arg_set_id': 'Extra args parsed from the proto message',
        }))

WINDOW_MANAGER_SHELL_TRANSITIONS_TABLE = Table(
    python_module=__file__,
    class_name='WindowManagerShellTransitionsTable',
    sql_name='window_manager_shell_transitions',
    columns=[
        C('ts', CppInt64()),
        C('transition_id', CppInt64()),
        C('arg_set_id', CppUint32()),
    ],
    tabledoc=TableDoc(
        doc='Window Manager Shell Transitions',
        group='Winscope',
        columns={
            'ts': 'The timestamp the transition started playing',
            'transition_id': 'The id of the transition',
            'arg_set_id': 'Extra args parsed from the proto message',
        }))

WINDOW_MANAGER_SHELL_TRANSITION_HANDLERS_TABLE = Table(
    python_module=__file__,
    class_name='WindowManagerShellTransitionHandlersTable',
    sql_name='window_manager_shell_transition_handlers',
    columns=[
        C('handler_id', CppInt64()),
        C('handler_name', CppString()),
    ],
    tabledoc=TableDoc(
        doc='Window Manager Shell Transition Handlers',
        group='Winscope',
        columns={
            'handler_id': 'The id of the handler',
            'handler_name': 'The name of the handler',
        }))

PROTOLOG_TABLE = Table(
    python_module=__file__,
    class_name='ProtoLogTable',
    sql_name='protolog',
    columns=[
        C('ts', CppInt64()),
        C('level', CppString()),
        C('tag', CppString()),
        C('message', CppString()),
        C('stacktrace', CppString()),
    ],
    tabledoc=TableDoc(
        doc='Protolog',
        group='Winscope',
        columns={
            'ts': 'The timestamp the log message was sent',
            'level': 'The log level of the protolog message',
            'tag': 'The log tag of the protolog message',
            'message': 'The protolog message',
            'stacktrace': 'Stacktrace captured at the message\'s logpoint',
        }))

# Keep this list sorted.
ALL_TABLES = [
    PROTOLOG_TABLE,
    SURFACE_FLINGER_LAYERS_SNAPSHOT_TABLE,
    SURFACE_FLINGER_LAYER_TABLE,
    SURFACE_FLINGER_TRANSACTIONS_TABLE,
    WINDOW_MANAGER_SHELL_TRANSITIONS_TABLE,
    WINDOW_MANAGER_SHELL_TRANSITION_HANDLERS_TABLE,
]
