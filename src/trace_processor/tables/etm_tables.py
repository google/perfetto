# Copyright (C) 2024 The Android Open Source Project
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
"""
Contains tables related to perf data ingestion.
"""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

ETM_V4_CONFIGURATION = Table(
    python_module=__file__,
    class_name='EtmV4ConfigurationTable',
    sql_name='__intrinsic_etm_v4_configuration',
    columns=[
        C('set_id', CppUint32(), flags=ColumnFlag.SORTED | ColumnFlag.SET_ID),
        C('cpu', CppUint32()),
        C('cs_trace_id', CppUint32()),
        C('core_profile', CppString()),
        C('arch_version', CppString()),
        C('major_version', CppUint32()),
        C('minor_version', CppUint32()),
        C('max_speculation_depth', CppUint32()),
        C('bool_flags', CppInt64()),
    ],
    tabledoc=TableDoc(
        doc='''
          This table tracks ETM configurations. Rows are grouped by a set_id
          to represent the configurations of each of the CPUs.
        ''',
        group='ETM',
        columns={
            'set_id':
                '''
                  Groups all configuration ros that belong to the same trace.
                  There is one row per each CPU where ETM was configured.
                ''',
            'cpu':
                'CPU this configuration applies to.',
            'cs_trace_id':
                'Trace Stream ID register',
            'core_profile':
                'Core Profile (e.g. Cortex-A or Cortex-M)',
            'arch_version':
                'Architecture version (e.g. AA64)',
            'major_version':
                'Major version',
            'minor_version':
                'Minor version',
            'max_speculation_depth':
                'Maximum speculation depth of the core',
            'bool_flags':
                'Collection of boolean flags.',
        },
    ))

ETM_V4_SESSION = Table(
    python_module=__file__,
    class_name='EtmV4SessionTable',
    sql_name='__intrinsic_etm_v4_session',
    columns=[
        C('configuration_id', CppTableId(ETM_V4_CONFIGURATION)),
        C('start_ts', CppOptional(CppInt64())),
    ],
    tabledoc=TableDoc(
        doc='''
          Represents a trace session on one core. From time the tracing is
          started to when it is stopped.
        ''',
        group='ETM',
        columns={
            'configuration_id':
                ColumnDoc(
                    'ETM configuration',
                    joinable='__intrinsic_etm_v4_configuration.id'),
            'start_ts':
                'time the trace ETM trace collection started.',
        },
    ))

ETM_V4_TRACE = Table(
    python_module=__file__,
    class_name='EtmV4TraceTable',
    sql_name='__intrinsic_etm_v4_trace',
    columns=[
        C('session_id', CppTableId(ETM_V4_SESSION)),
        C('trace_set_id',
          CppUint32(),
          flags=ColumnFlag.SORTED | ColumnFlag.SET_ID),
        C('size', CppInt64()),
    ],
    tabledoc=TableDoc(
        doc='''
          Represents a contiguous chunk of ETM trace data for a core. The data
          collected during a session might be split into different chunks in the
          case of data loss.
        ''',
        group='ETM',
        columns={
            'session_id':
                ColumnDoc(
                    'Session this data belongs to',
                    joinable='__intrinsic_etm_v4_trace.id'),
            'trace_set_id':
                'Groups all the traces belonging to the same session.',
            'size':
                'Size in bytes',
        },
    ))

ALL_TABLES = [ETM_V4_CONFIGURATION, ETM_V4_TRACE, ETM_V4_SESSION]
