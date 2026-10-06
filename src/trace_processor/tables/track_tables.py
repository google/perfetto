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
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppAccessDuration
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppSelfTableId
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import SqlAccess
from python.generators.trace_processor_table.public import Table

from src.trace_processor.tables.metadata_tables import MACHINE_TABLE
from src.trace_processor.tables.metadata_tables import THREAD_TABLE
from src.trace_processor.tables.metadata_tables import PROCESS_TABLE

TRACK_TABLE = Table(
    python_module=__file__,
    class_name="TrackTable",
    sql_name="__intrinsic_track",
    columns=[
        C(
            "name",
            CppString(),
            cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C(
            "parent_id",
            CppOptional(CppSelfTableId()),
            sql_access=SqlAccess.HIGH_PERF,
            cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C(
            "source_arg_set_id",
            CppOptional(CppUint32()),
            sql_access=SqlAccess.HIGH_PERF,
            cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C('machine_id', CppTableId(MACHINE_TABLE)),
        C(
            "type",
            CppString(),
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C(
            "dimension_arg_set_id",
            CppOptional(CppUint32()),
            sql_access=SqlAccess.HIGH_PERF,
            cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C(
            "track_group_id",
            CppOptional(CppUint32()),
            sql_access=SqlAccess.HIGH_PERF,
            cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE,
        ),
        C("event_type", CppString()),
        C("counter_unit", CppOptional(CppString())),
        C(
            "utid",
            CppOptional(CppTableId(THREAD_TABLE)),
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
        C(
            "upid",
            CppOptional(CppTableId(PROCESS_TABLE)),
            cpp_access=CppAccess.READ,
            cpp_access_duration=CppAccessDuration.POST_FINALIZATION,
        ),
    ])

# The dimensions of every track: both the well known dimensions (machine,
# process, thread, cpu, gpu) of every track, written when the track is created,
# and the custom dimensions declared by producers on TrackDescriptors, written
# when the track event track is created, after process/thread association and
# `parent_id` inheritance have been applied.
TRACK_DIMENSION_TABLE = Table(
    python_module=__file__,
    class_name="TrackDimensionTable",
    sql_name="__intrinsic_track_dimension",
    columns=[
        C(
            "track_id",
            CppTableId(TRACK_TABLE),
            cpp_access=CppAccess.READ,
        ),
        C(
            "name",
            CppString(),
            cpp_access=CppAccess.READ,
        ),
        C(
            "int_value",
            CppOptional(CppInt64()),
            cpp_access=CppAccess.READ,
        ),
        C(
            "string_value",
            CppOptional(CppString()),
            cpp_access=CppAccess.READ,
        ),
        C(
            "display_name",
            CppOptional(CppString()),
            cpp_access=CppAccess.READ,
        ),
        # 1 for well known dimensions, 0 for custom ones.
        C(
            "is_well_known",
            CppUint32(),
            cpp_access=CppAccess.READ,
        ),
    ])

# The custom dimensions declared by producers on the root TrackDescriptor of a
# process. They apply to every track and thread of the process.
PROCESS_DIMENSION_TABLE = Table(
    python_module=__file__,
    class_name="ProcessDimensionTable",
    sql_name="__intrinsic_process_dimension",
    columns=[
        C(
            "upid",
            CppTableId(PROCESS_TABLE),
            cpp_access=CppAccess.READ,
        ),
        C(
            "name",
            CppString(),
            cpp_access=CppAccess.READ,
        ),
        C(
            "int_value",
            CppOptional(CppInt64()),
            cpp_access=CppAccess.READ,
        ),
        C(
            "string_value",
            CppOptional(CppString()),
            cpp_access=CppAccess.READ,
        ),
        C(
            "display_name",
            CppOptional(CppString()),
            cpp_access=CppAccess.READ,
        ),
    ])

# The custom dimensions declared by producers on the root TrackDescriptor of a
# thread, excluding the ones it already inherits from its process. They apply
# to every track of the thread.
THREAD_DIMENSION_TABLE = Table(
    python_module=__file__,
    class_name="ThreadDimensionTable",
    sql_name="__intrinsic_thread_dimension",
    columns=[
        C(
            "utid",
            CppTableId(THREAD_TABLE),
            cpp_access=CppAccess.READ,
        ),
        C(
            "name",
            CppString(),
            cpp_access=CppAccess.READ,
        ),
        C(
            "int_value",
            CppOptional(CppInt64()),
            cpp_access=CppAccess.READ,
        ),
        C(
            "string_value",
            CppOptional(CppString()),
            cpp_access=CppAccess.READ,
        ),
        C(
            "display_name",
            CppOptional(CppString()),
            cpp_access=CppAccess.READ,
        ),
    ])

# Keep this list sorted.
ALL_TABLES = [
    PROCESS_DIMENSION_TABLE,
    THREAD_DIMENSION_TABLE,
    TRACK_DIMENSION_TABLE,
    TRACK_TABLE,
]
