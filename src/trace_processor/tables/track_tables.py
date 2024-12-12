# Copyright (C) 2022 The Android Open Source Project
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
"""Contains tables for tracks."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppSelfTableId
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

from src.trace_processor.tables.metadata_tables import MACHINE_TABLE

TRACK_TABLE = Table(
    python_module=__file__,
    class_name="TrackTable",
    sql_name="__intrinsic_track",
    columns=[
        C("name", CppString()),
        C("parent_id", CppOptional(CppSelfTableId())),
        C("source_arg_set_id", CppOptional(CppUint32())),
        C('machine_id', CppOptional(CppTableId(MACHINE_TABLE))),
        C("classification", CppString()),
        C("dimension_arg_set_id", CppOptional(CppUint32())),
        C("event_type", CppString()),
        C("counter_unit", CppOptional(CppString())),
    ])

PROCESS_TRACK_TABLE = Table(
    python_module=__file__,
    class_name="ProcessTrackTable",
    sql_name="process_track",
    columns=[
        C("upid", CppUint32()),
    ],
    parent=TRACK_TABLE,
    tabledoc=TableDoc(
        doc='''
          Tracks which are associated to the process given by the |upid| column
        ''',
        group='Tracks',
        columns={
            'upid':
                ColumnDoc(
                    doc='The process associated with this track.',
                    joinable='process.upid'),
        }))

THREAD_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='ThreadTrackTable',
    sql_name='thread_track',
    columns=[
        C('utid', CppUint32()),
    ],
    parent=TRACK_TABLE,
    tabledoc=TableDoc(
        doc='''
          Tracks which are associated to the thread given by the |utid| column
        ''',
        group='Tracks',
        columns={
            'utid':
                ColumnDoc(
                    doc='The thread associated with this track',
                    joinable='thread.utid',
                )
        }))

CPU_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='CpuTrackTable',
    sql_name='__intrinsic_cpu_track',
    columns=[
        C('cpu', CppUint32()),
    ],
    parent=TRACK_TABLE)

GPU_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='GpuTrackTable',
    sql_name='gpu_track',
    columns=[
        C('scope', CppString()),
        C('description', CppString()),
        C('context_id', CppOptional(CppInt64())),
    ],
    parent=TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Tracks associated to a GPU.',
        group='Tracks',
        columns={
            'scope':
                'The scope for the track. For debugging purposes only.',
            'description':
                'The description of the track. For debugging purposes only.',
            'context_id':
                'The context id for the GPU this track is associated to.'
        }))

# Keep this list sorted.
ALL_TABLES = [
    CPU_TRACK_TABLE,
    GPU_TRACK_TABLE,
    PROCESS_TRACK_TABLE,
    THREAD_TRACK_TABLE,
    TRACK_TABLE,
]
