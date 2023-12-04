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
from python.generators.trace_processor_table.public import CppInt32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import CppSelfTableId
from python.generators.trace_processor_table.public import CppUint32

TRACK_TABLE = Table(
    python_module=__file__,
    class_name="TrackTable",
    sql_name="track",
    columns=[
        C("name", CppString()),
        C("parent_id", CppOptional(CppSelfTableId())),
        C("source_arg_set_id", CppOptional(CppUint32())),
    ],
    tabledoc=TableDoc(
        doc='''
          Tracks are a fundamental concept in trace processor and represent a
          "timeline" for events of the same type and with the same context. See
          https://perfetto.dev/docs/analysis/trace-processor#tracks for a more
          detailed explanation, with examples.
        ''',
        group='Tracks',
        columns={
            'name':
                '''
                  Name of the track; can be null for some types of tracks (e.g.
                  thread tracks).
                ''',
            'parent_id':
                '''
                  The track which is the "parent" of this track. Only non-null
                  for tracks created using Perfetto's track_event API.
                ''',
            'source_arg_set_id':
                ColumnDoc(
                    doc='''
                      Args for this track which store information about "source"
                      of this track in the trace. For example: whether this
                      track orginated from atrace, Chrome tracepoints etc.
                    ''',
                    joinable='args.arg_set_id'),
        }))

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
    sql_name='cpu_track',
    columns=[
        C('cpu', CppUint32()),
    ],
    parent=TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Tracks which are associated to a single CPU',
        group='Tracks',
        columns={'cpu': 'The CPU associated with this track'}))

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
        doc='Tracks assocaited to a GPU.',
        group='Tracks',
        columns={
            'scope':
                'The scope for the track. For debugging purposes only.',
            'description':
                'The description of the track. For debugging purposes only.',
            'context_id':
                'The context id for the GPU this track is associated to.'
        }))

COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='CounterTrackTable',
    sql_name='counter_track',
    columns=[
        C('unit', CppString()),
        C('description', CppString()),
    ],
    parent=TRACK_TABLE,
    tabledoc=TableDoc(
        doc='''
          Tracks containing counter-like events. See
          https://perfetto.dev/docs/analysis/trace-processor#events for a
          defintion and examples of counters.
        ''',
        group='Tracks',
        columns={
            'unit':
                'The units of the counter. This column is rarely filled.',
            'description':
                'The description for this track. For debugging purposes only.'
        }))

THREAD_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='ThreadCounterTrackTable',
    sql_name='thread_counter_track',
    columns=[
        C('utid', CppUint32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Tracks containing counter-like events associated to a thread.',
        group='Counter Tracks',
        columns={
            'utid':
                ColumnDoc(
                    doc='The thread associated with this track',
                    joinable='thread.utid',
                )
        }))

PROCESS_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='ProcessCounterTrackTable',
    sql_name='process_counter_track',
    columns=[
        C('upid', CppUint32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='''
          Tracks containing counter-like events associated to a process.
        ''',
        group='Counter Tracks',
        columns={
            'upid':
                ColumnDoc(
                    doc='The process associated with this track',
                    joinable='process.upid')
        }))

CPU_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='CpuCounterTrackTable',
    sql_name='cpu_counter_track',
    columns=[
        C('cpu', CppUint32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Tracks containing counter-like events associated to a CPU.',
        group='Counter Tracks',
        columns={'cpu': 'The CPU this track is associated with'}))

IRQ_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='IrqCounterTrackTable',
    sql_name='irq_counter_track',
    columns=[
        C('irq', CppInt32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Tracks containing counter-like events associated to an hardirq',
        group='Counter Tracks',
        columns={'irq': 'The identifier for the hardirq.'}))

SOFTIRQ_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='SoftirqCounterTrackTable',
    sql_name='softirq_counter_track',
    columns=[
        C('softirq', CppInt32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Tracks containing counter-like events associated to a softirq',
        group='Counter Tracks',
        columns={'softirq': 'The identifier for the softirq.'}))

GPU_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='GpuCounterTrackTable',
    sql_name='gpu_counter_track',
    columns=[
        C('gpu_id', CppUint32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Tracks containing counter-like events associated to a GPU',
        group='Counter Tracks',
        columns={'gpu_id': 'The identifier for the GPU.'}))

PERF_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='PerfCounterTrackTable',
    sql_name='perf_counter_track',
    columns=[
        C('perf_session_id', CppUint32()),
        C('cpu', CppUint32()),
        C('is_timebase', CppUint32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Sampled counters\' values for samples in the perf_sample table.',
        group='Counter Tracks',
        columns={
            'perf_session_id':
                'id of a distict profiling stream',
            'cpu':
                'the core the sample was taken on',
            'is_timebase':
                '''
                  If true, indicates this counter was the sampling timebase for
                  this perf_session_id
                '''
        }))

ENERGY_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='EnergyCounterTrackTable',
    sql_name='energy_counter_track',
    columns=[
        C('consumer_id', CppInt32()),
        C('consumer_type', CppString()),
        C('ordinal', CppInt32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='''
          Energy consumers' values for energy descriptors in
          energy_estimation_breakdown packet
        ''',
        group='Counter Tracks',
        columns={
            'consumer_id': 'id of a distinct energy consumer',
            'consumer_type': 'type of energy consumer',
            'ordinal': 'ordinal of energy consumer'
        }))

UID_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='UidCounterTrackTable',
    sql_name='uid_counter_track',
    columns=[
        C('uid', CppInt32()),
    ],
    parent=COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='The uid associated with this track',
        group='Counter Tracks',
        columns={'uid': 'uid of process for which breakdowns are emitted'}))

ENERGY_PER_UID_COUNTER_TRACK_TABLE = Table(
    python_module=__file__,
    class_name='EnergyPerUidCounterTrackTable',
    sql_name='energy_per_uid_counter_track',
    columns=[
        C('consumer_id', CppInt32()),
    ],
    parent=UID_COUNTER_TRACK_TABLE,
    tabledoc=TableDoc(
        doc='Energy consumer values for per uid in uid_counter_track',
        group='Counter Tracks',
        columns={'consumer_id': 'id of the consumer process'}))

# Keep this list sorted.
ALL_TABLES = [
    COUNTER_TRACK_TABLE,
    CPU_COUNTER_TRACK_TABLE,
    CPU_TRACK_TABLE,
    ENERGY_COUNTER_TRACK_TABLE,
    ENERGY_PER_UID_COUNTER_TRACK_TABLE,
    GPU_COUNTER_TRACK_TABLE,
    GPU_TRACK_TABLE,
    IRQ_COUNTER_TRACK_TABLE,
    PERF_COUNTER_TRACK_TABLE,
    PROCESS_COUNTER_TRACK_TABLE,
    PROCESS_TRACK_TABLE,
    SOFTIRQ_COUNTER_TRACK_TABLE,
    THREAD_COUNTER_TRACK_TABLE,
    THREAD_TRACK_TABLE,
    TRACK_TABLE,
    UID_COUNTER_TRACK_TABLE,
]
