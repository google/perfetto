# Copyright (C) 2022 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the 'License');
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an 'AS IS' BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Contains metadata tables for a wide range of usecases."""

from python.generators.trace_processor_table.public import Alias
from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import CppDouble
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import CppSelfTableId
from python.generators.trace_processor_table.public import WrappingSqlView

PROCESS_TABLE = Table(
    python_module=__file__,
    class_name='ProcessTable',
    sql_name='internal_process',
    columns=[
        C('upid', Alias(underlying_column='id')),
        C('pid', CppUint32()),
        C('name', CppOptional(CppString())),
        C('start_ts', CppOptional(CppInt64())),
        C('end_ts', CppOptional(CppInt64())),
        C('parent_upid', CppOptional(CppSelfTableId())),
        C('uid', CppOptional(CppUint32())),
        C('android_appid', CppOptional(CppUint32())),
        C('cmdline', CppOptional(CppString())),
        C('arg_set_id', CppUint32()),
    ],
    wrapping_sql_view=WrappingSqlView(view_name='process',),
    tabledoc=TableDoc(
        doc='Contains information of processes seen during the trace',
        group='Metadata',
        skip_id_and_type=True,
        columns={
            'upid':
                '''
                   Unique process id. This is != the OS pid. This is a
                   monotonic number associated to each process. The OS process
                   id (pid) cannot be used as primary key because tids and pids
                   are recycled by most kernels.
                ''',
            'pid':
                '''
                  The OS id for this process. Note: this is *not* unique
                  over the lifetime of the trace so cannot be used as a
                  primary key. Use |upid| instead.
                ''',
            'name':
                '''
                  The name of the process. Can be populated from many sources
                  (e.g. ftrace, /proc scraping, track event etc).
                ''',
            'start_ts':
                '''
                  The start timestamp of this process (if known). Is null
                  in most cases unless a process creation event is enabled
                  (e.g. task_newtask ftrace event on Linux/Android).
                ''',
            'end_ts':
                '''
                  The end timestamp of this process (if known). Is null in
                  most cases unless a process destruction event is enabled
                  (e.g. sched_process_free ftrace event on Linux/Android).
                ''',
            'parent_upid':
                ColumnDoc(
                    '''
                  The upid of the process which caused this process to be
                  spawned.
                ''',
                    joinable='process.upid'),
            'uid':
                ColumnDoc(
                    'The Unix user id of the process.',
                    joinable='package_list.uid'),
            'android_appid':
                'Android appid of this process.',
            'cmdline':
                '/proc/cmdline for this process.',
            'arg_set_id':
                ColumnDoc(
                    'Extra args for this process.', joinable='args.arg_set_id'),
        }))

THREAD_TABLE = Table(
    python_module=__file__,
    class_name='ThreadTable',
    sql_name='internal_thread',
    columns=[
        C('utid', Alias(underlying_column='id')),
        C('tid', CppUint32()),
        C('name', CppOptional(CppString())),
        C('start_ts', CppOptional(CppInt64())),
        C('end_ts', CppOptional(CppInt64())),
        C('upid', CppOptional(CppTableId(PROCESS_TABLE))),
        C('is_main_thread', CppOptional(CppUint32())),
    ],
    wrapping_sql_view=WrappingSqlView(view_name='thread',),
    tabledoc=TableDoc(
        doc='Contains information of threads seen during the trace',
        group='Metadata',
        skip_id_and_type=True,
        columns={
            'utid':
                '''
                  Unique thread id. This is != the OS tid. This is a monotonic
                  number associated to each thread. The OS thread id (tid)
                  cannot be used as primary key because tids and pids are
                  recycled by most kernels.
                ''',
            'tid':
                '''
                  The OS id for this thread. Note: this is *not* unique over the
                  lifetime of the trace so cannot be used as a primary key. Use
                  |utid| instead.
                ''',
            'name':
                '''
                  The name of the thread. Can be populated from many sources
                  (e.g. ftrace, /proc scraping, track event etc).
                ''',
            'start_ts':
                '''
                  The start timestamp of this thread (if known). Is null in most
                  cases unless a thread creation event is enabled (e.g.
                  task_newtask ftrace event on Linux/Android).
                ''',
            'end_ts':
                '''
                  The end timestamp of this thread (if known). Is null in most
                  cases unless a thread destruction event is enabled (e.g.
                  sched_process_free ftrace event on Linux/Android).
                ''',
            'upid':
                ColumnDoc(
                    'The process hosting this thread.',
                    joinable='process.upid'),
            'is_main_thread':
                '''
                  Boolean indicating if this thread is the main thread
                  in the process.
                '''
        }))

RAW_TABLE = Table(
    python_module=__file__,
    class_name='RawTable',
    sql_name='raw',
    columns=[
        C('ts', CppInt64(), flags=ColumnFlag.SORTED),
        C('name', CppString()),
        C('cpu', CppUint32()),
        C('utid', CppTableId(THREAD_TABLE)),
        C('arg_set_id', CppUint32()),
        C('common_flags', CppUint32())
    ],
    tabledoc=TableDoc(
        doc='''
          Contains 'raw' events from the trace for some types of events. This
          table only exists for debugging purposes and should not be relied on
          in production usecases (i.e. metrics, standard library etc).
        ''',
        group='Events',
        columns={
            'arg_set_id':
                ColumnDoc(
                    'The set of key/value pairs associated with this event.',
                    joinable='args.arg_set_id'),
            'ts':
                'The timestamp of this event.',
            'name':
                '''
                  The name of the event. For ftrace events, this will be the
                  ftrace event name.
                ''',
            'cpu':
                'The CPU this event was emitted on.',
            'utid':
                'The thread this event was emitted on.',
            'common_flags':
                'Ftrace event flags for this event. Currently only emitted for sched_waking events.'
        }))

FTRACE_EVENT_TABLE = Table(
    python_module=__file__,
    class_name='FtraceEventTable',
    sql_name='ftrace_event',
    parent=RAW_TABLE,
    columns=[],
    tabledoc=TableDoc(
        doc='''
          Contains all the ftrace events in the trace. This table exists only
          for debugging purposes and should not be relied on in production
          usecases (i.e. metrics, standard library etc). Note also that this
          table might be empty if raw ftrace parsing has been disabled.
        ''',
        group='Events',
        columns={}))

ARG_TABLE = Table(
    python_module=__file__,
    class_name='ArgTable',
    sql_name='internal_args',
    columns=[
        C('arg_set_id', CppUint32(), flags=ColumnFlag.SORTED),
        C('flat_key', CppString()),
        C('key', CppString()),
        C('int_value', CppOptional(CppInt64())),
        C('string_value', CppOptional(CppString())),
        C('real_value', CppOptional(CppDouble())),
        C('value_type', CppString()),
    ],
    wrapping_sql_view=WrappingSqlView(view_name='args'),
    tabledoc=TableDoc(
        doc='''''',
        group='Misc',
        columns={
            'arg_set_id': '''''',
            'flat_key': '''''',
            'key': '''''',
            'int_value': '''''',
            'string_value': '''''',
            'real_value': '''''',
            'value_type': ''''''
        }))

METADATA_TABLE = Table(
    python_module=__file__,
    class_name='MetadataTable',
    sql_name='metadata',
    columns=[
        C('name', CppString()),
        C('key_type', CppString()),
        C('int_value', CppOptional(CppInt64())),
        C('str_value', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''''',
        group='Metadata',
        columns={
            'name': '''''',
            'key_type': '''''',
            'int_value': '''''',
            'str_value': ''''''
        }))

FILEDESCRIPTOR_TABLE = Table(
    python_module=__file__,
    class_name='FiledescriptorTable',
    sql_name='filedescriptor',
    columns=[
        C('ufd', CppInt64()),
        C('fd', CppInt64()),
        C('ts', CppOptional(CppInt64())),
        C('upid', CppOptional(CppUint32())),
        C('path', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          Contains information of filedescriptors collected during the trace
        ''',
        group='Metadata',
        columns={
            'ufd':
                '''Unique fd. This is != the OS fd.
This is a monotonic number associated to each
filedescriptor. The OS assigned fd cannot be used as
primary key because fds are recycled by most kernels.''',
            'fd':
                '''The OS id for this process. Note: this is *not*
unique over the lifetime of the trace so cannot be
used as a primary key. Use |ufd| instead.''',
            'ts':
                '''The timestamp for when the fd was collected.''',
            'upid':
                ''' The upid of the process which
opened the filedescriptor.''',
            'path':
                '''The path to the file or device backing the fd
In case this was a socket the path will be the port
number.'''
        }))

EXP_MISSING_CHROME_PROC_TABLE = Table(
    python_module=__file__,
    class_name='ExpMissingChromeProcTable',
    sql_name='experimental_missing_chrome_processes',
    columns=[
        C('upid', CppUint32()),
        C('reliable_from', CppOptional(CppInt64())),
    ],
    tabledoc=TableDoc(
        doc='''
          Experimental table, subject to arbitrary breaking changes.
        ''',
        group='Chrome',
        columns={
            'upid': '''''',
            'reliable_from': ''''''
        }))

CPU_TABLE = Table(
    python_module=__file__,
    class_name='CpuTable',
    sql_name='cpu',
    columns=[
        C('cluster_id', CppUint32()),
        C('processor', CppString()),
    ],
    tabledoc=TableDoc(
        doc='''
          Contains information of processes seen during the trace
        ''',
        group='Misc',
        columns={
            'cluster_id':
                '''the cluster id is shared by CPUs in
the same cluster''',
            'processor':
                '''a string describing this core'''
        }))

CPU_FREQ_TABLE = Table(
    python_module=__file__,
    class_name='CpuFreqTable',
    sql_name='cpu_freq',
    columns=[
        C('cpu_id', CppTableId(CPU_TABLE)),
        C('freq', CppUint32()),
    ],
    tabledoc=TableDoc(
        doc='''''', group='Misc', columns={
            'cpu_id': '''''',
            'freq': ''''''
        }))

CLOCK_SNAPSHOT_TABLE = Table(
    python_module=__file__,
    class_name='ClockSnapshotTable',
    sql_name='clock_snapshot',
    columns=[
        C('ts', CppInt64()),
        C('clock_id', CppInt64()),
        C('clock_name', CppOptional(CppString())),
        C('clock_value', CppInt64()),
        C('snapshot_id', CppUint32()),
    ],
    tabledoc=TableDoc(
        doc='''
          Contains all the mapping between clock snapshots and trace time.

NOTE: this table is not sorted by timestamp; this is why we omit the
sorted flag on the ts column.
        ''',
        group='Misc',
        columns={
            'ts':
                '''timestamp of the snapshot in trace time.''',
            'clock_id':
                '''id of the clock (corresponds to the id in the trace).''',
            'clock_name':
                '''the name of the clock for builtin clocks or null
otherwise.''',
            'clock_value':
                '''timestamp of the snapshot in clock time.''',
            'snapshot_id':
                '''the index of this snapshot (only useful for debugging)'''
        }))

# Keep this list sorted.
ALL_TABLES = [
    ARG_TABLE,
    CLOCK_SNAPSHOT_TABLE,
    CPU_FREQ_TABLE,
    CPU_TABLE,
    EXP_MISSING_CHROME_PROC_TABLE,
    FILEDESCRIPTOR_TABLE,
    METADATA_TABLE,
    PROCESS_TABLE,
    RAW_TABLE,
    THREAD_TABLE,
    FTRACE_EVENT_TABLE,
]
