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
"""Contains tables for relevant for Android."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from src.trace_processor.tables.metadata_tables import THREAD_TABLE

ANDROID_LOG_TABLE = Table(
    class_name="AndroidLogTable",
    sql_name="android_logs",
    columns=[
        C("ts", CppInt64()),
        C("utid", CppTableId(THREAD_TABLE)),
        C("prio", CppUint32()),
        C("tag", CppOptional(CppString())),
        C("msg", CppString()),
    ],
    tabledoc=TableDoc(
        doc='''
          Log entries from Android logcat.

          NOTE: this table is not sorted by timestamp. This is why we omit the
          sorted flag on the ts column.
        ''',
        group='Events',
        columns={
            'ts': 'Timestamp of log entry.',
            'utid': 'Thread writing the log entry.',
            'prio': 'Priority of the log. 3=DEBUG, 4=INFO, 5=WARN, 6=ERROR.',
            'tag': 'Tag of the log entry.',
            'msg': 'Content of the log entry.'
        }))

# Keep this list sorted.
ALL_TABLES = [
    ANDROID_LOG_TABLE,
]
