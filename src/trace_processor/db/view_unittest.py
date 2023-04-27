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
"""Contains tables for unitviewing."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppString

VIEW_THREAD_TABLE = Table(
    python_module=__file__,
    class_name="ViewThreadTable",
    sql_name="thread_table",
    columns=[
        C("name", CppString()),
        C("tid", CppUint32()),
    ])

VIEW_TRACK_TABLE = Table(
    python_module=__file__,
    class_name="ViewTrackTable",
    sql_name="track_table",
    columns=[
        C("name", CppString()),
    ])

VIEW_THREAD_TRACK_TABLE = Table(
    python_module=__file__,
    class_name="ViewThreadTrackTable",
    sql_name="thread_track_table",
    parent=VIEW_TRACK_TABLE,
    columns=[
        C("utid", CppTableId(VIEW_THREAD_TABLE)),
    ])

VIEW_EVENT_TABLE = Table(
    python_module=__file__,
    class_name="ViewEventTable",
    sql_name="event_table",
    columns=[
        C("ts", CppInt64(), flags=ColumnFlag.SORTED),
        C("track_id", CppTableId(VIEW_TRACK_TABLE)),
    ])

VIEW_SLICE_TABLE = Table(
    python_module=__file__,
    class_name="ViewSliceTable",
    sql_name="slice_table",
    parent=VIEW_EVENT_TABLE,
    columns=[
        C("name", CppString()),
    ])

# Keep this list sorted.
ALL_TABLES = [
    VIEW_EVENT_TABLE,
    VIEW_SLICE_TABLE,
    VIEW_THREAD_TABLE,
    VIEW_THREAD_TRACK_TABLE,
    VIEW_TRACK_TABLE,
]
