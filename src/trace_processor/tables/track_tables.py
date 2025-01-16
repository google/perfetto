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
from src.trace_processor.tables.metadata_tables import THREAD_TABLE
from src.trace_processor.tables.metadata_tables import PROCESS_TABLE

TRACK_TABLE = Table(
    python_module=__file__,
    class_name="TrackTable",
    sql_name="__intrinsic_track",
    columns=[
        C("name", CppString()),
        C("parent_id", CppOptional(CppSelfTableId())),
        C("source_arg_set_id", CppOptional(CppUint32())),
        C('machine_id', CppOptional(CppTableId(MACHINE_TABLE))),
        C("type", CppString()),
        C("dimension_arg_set_id", CppOptional(CppUint32())),
        C("event_type", CppString()),
        C("counter_unit", CppOptional(CppString())),
        C("utid", CppOptional(CppTableId(THREAD_TABLE))),
        C("upid", CppOptional(CppTableId(PROCESS_TABLE))),
    ])

# Keep this list sorted.
ALL_TABLES = [
    TRACK_TABLE,
]
