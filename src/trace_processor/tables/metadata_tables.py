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
        group='Misc',
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
                '''
                  The upid of the process which caused this process to be
                  spawned.
                ''',
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
        group='Misc',
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
                'The process hosting this thread.',
            'is_main_thread':
                '''
                  Boolean indicating if this thread is the main thread
                  in the process.
                '''
        }))

# Keep this list sorted.
ALL_TABLES = [
    THREAD_TABLE,
    PROCESS_TABLE,
]
