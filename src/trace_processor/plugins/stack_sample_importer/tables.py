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
"""Tables owned by the transport-neutral stack sampling importer plugin."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppAccessDuration
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

from src.trace_processor.tables.profiler_tables import STACK_PROFILE_CALLSITE_TABLE

STACK_SAMPLE_TASK_CONTEXT_TABLE = Table(
    python_module=__file__,
    class_name='StackSampleTaskContextTable',
    sql_name='__intrinsic_stack_sample_task_context',
    columns=[
        C('utid', CppOptional(CppUint32())),
        C('upid', CppOptional(CppUint32())),
        C('async_name', CppOptional(CppString())),
        C('async_kind', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          The task a stack sample is attributed to: a thread, a process, and/or
          a stackful async context (goroutine, fiber, ...). Deduplicated, one
          row per distinct task.
        ''',
        group='Callstack profilers',
        columns={
            'utid':
                '''The sampled thread, if the task is a thread. Joinable with
                   thread.utid.''',
            'upid':
                '''The sampled process, if known. Joinable with
                   process.upid.''',
            'async_name':
                '''Name of the async context (from AsyncContextDescriptor), if
                   the sample is attributed to one.''',
            'async_kind':
                '''Kind of the async context, e.g. "goroutine", "fiber".''',
        }))

STACK_SAMPLE_EXECUTION_CONTEXT_TABLE = Table(
    python_module=__file__,
    class_name='StackSampleExecutionContextTable',
    sql_name='__intrinsic_stack_sample_execution_context',
    columns=[
        C('cpu', CppOptional(CppUint32())),
        C('mode', CppString()),
    ],
    tabledoc=TableDoc(
        doc='''
          The execution state a stack sample was taken in. Deduplicated, one row
          per distinct state.
        ''',
        group='Callstack profilers',
        columns={
            'cpu':
                '''Core the sample was taken on, if known.''',
            'mode':
                '''Privilege mode the sample was taken in (e.g. "user",
                   "kernel"). Empty if unknown.''',
        }))

STACK_SAMPLE_COUNTER_TABLE = Table(
    python_module=__file__,
    class_name='StackSampleCounterTable',
    sql_name='__intrinsic_stack_sample_counter',
    columns=[
        C('source', CppString()),
        C('name', CppString()),
        C('unit', CppOptional(CppString())),
        C('unit_multiplier', CppOptional(CppInt64())),
        C('description', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          A counter a stack sample is measured against: the profiler source and
          a CounterDescriptor. Used both for the primary timebase and for
          follower counters. Deduplicated, one row per distinct counter.
        ''',
        group='Callstack profilers',
        columns={
            'source':
                '''The profiler that produced the samples (from
                   StackSampleDefaults.source, e.g. "linux.perf").''',
            'name':
                '''The counter name (e.g. "wall-time", "cycles").''',
            'unit':
                '''The unit weights are expressed in, if known.''',
            'unit_multiplier':
                '''Scales raw weights to `unit`, if set.''',
            'description':
                '''Human-readable description of the counter, if provided.''',
        }))

STACK_SAMPLE_TABLE = Table(
    python_module=__file__,
    class_name='StackSampleTable',
    sql_name='__intrinsic_stack_sample',
    columns=[
        C(
            'ts',
            CppInt64(),
            flags=ColumnFlag.SORTED,
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C('task_context_id',
          CppOptional(CppTableId(STACK_SAMPLE_TASK_CONTEXT_TABLE))),
        C('execution_context_id',
          CppOptional(CppTableId(STACK_SAMPLE_EXECUTION_CONTEXT_TABLE))),
        C('timebase_id', CppTableId(STACK_SAMPLE_COUNTER_TABLE)),
        C('callsite_id', CppOptional(CppTableId(STACK_PROFILE_CALLSITE_TABLE))),
        C('weight', CppOptional(CppInt64())),
    ],
    tabledoc=TableDoc(
        doc='''Transport-neutral, timebase-neutral stack samples (StackSample).
               Each row is a callstack captured for a task and measured against
               a primary counter timebase.''',
        group='Callstack profilers',
        columns={
            'ts':
                '''Timestamp of the sample.''',
            'task_context_id':
                '''The task this sample is attributed to, if any.''',
            'execution_context_id':
                '''The execution state (cpu, privilege mode) at sample time, if
                   known.''',
            'timebase_id':
                '''The primary counter (timebase) this sample is measured
                   against.''',
            'callsite_id':
                '''If set, the captured callstack.''',
            'weight':
                '''Value attributed to this sample for the timebase counter, if
                   set.''',
        }))

STACK_SAMPLE_FOLLOWER_TABLE = Table(
    python_module=__file__,
    class_name='StackSampleFollowerTable',
    sql_name='__intrinsic_stack_sample_follower',
    columns=[
        C('stack_sample_id', CppTableId(STACK_SAMPLE_TABLE)),
        C('counter_id', CppTableId(STACK_SAMPLE_COUNTER_TABLE)),
        C('weight', CppInt64()),
    ],
    tabledoc=TableDoc(
        doc='''A follower counter value recorded alongside a stack sample: an
               additional counter (e.g. instructions) read at the same sample
               point as the primary timebase.''',
        group='Callstack profilers',
        columns={
            'stack_sample_id': '''The sample this follower value belongs to.''',
            'counter_id': '''The follower counter this value is for.''',
            'weight': '''The follower counter value at this sample.''',
        }))

# Keep this list sorted.
ALL_TABLES = [
    STACK_SAMPLE_COUNTER_TABLE,
    STACK_SAMPLE_EXECUTION_CONTEXT_TABLE,
    STACK_SAMPLE_FOLLOWER_TABLE,
    STACK_SAMPLE_TABLE,
    STACK_SAMPLE_TASK_CONTEXT_TABLE,
]
