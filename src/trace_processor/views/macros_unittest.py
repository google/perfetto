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
"""Contains tables for unittesting."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId

MACROS_THREAD_TABLE = Table(
    python_module=__file__,
    class_name="MacrosThreadTable",
    sql_name="thread",
    columns=[
        C("name", CppString()),
        C("start_ts", CppInt64(), flags=ColumnFlag.SORTED),
    ])

MACROS_EVENT_TABLE = Table(
    python_module=__file__,
    class_name="MacrosEventTable",
    sql_name="event",
    columns=[
        C("ts", CppInt64(), flags=ColumnFlag.SORTED),
        C("thread_id", CppTableId(MACROS_THREAD_TABLE)),
    ])

# Keep this list sorted.
ALL_TABLES = [
    MACROS_THREAD_TABLE,
    MACROS_EVENT_TABLE,
]
