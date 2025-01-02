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
"""Contains tables for unittesting."""

from python.generators.trace_processor_table.public import Column as C, CppOptional
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import CppUint32

EVENT_TABLE = Table(
    python_module=__file__,
    class_name="TestEventTable",
    sql_name="event",
    columns=[
        C("ts", CppInt64(), flags=ColumnFlag.SORTED),
        C("arg_set_id", CppOptional(CppUint32())),
    ])

EVENT_CHILD_TABLE = Table(
    python_module=__file__,
    class_name="TestEventChildTable",
    sql_name="event",
    parent=EVENT_TABLE,
    columns=[])

SLICE_TABLE = Table(
    python_module=__file__,
    class_name="TestSliceTable",
    sql_name="slice",
    parent=EVENT_TABLE,
    columns=[
        C("dur", CppInt64()),
    ])

ARGS_TABLE = Table(
    python_module=__file__,
    class_name="TestArgsTable",
    sql_name="args",
    columns=[
        C("arg_set_id",
          CppUint32(),
          flags=ColumnFlag.SET_ID | ColumnFlag.SORTED),
        C("int_value", CppInt64()),
    ])

# Keep this list sorted.
ALL_TABLES = [
    ARGS_TABLE,
    EVENT_TABLE,
    EVENT_CHILD_TABLE,
    SLICE_TABLE,
]
