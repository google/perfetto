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
"""Contains tables for finding ancestor events."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import CppDouble
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import Table

from src.trace_processor.tables.counter_tables import COUNTER_TABLE
from src.trace_processor.tables.flow_tables import FLOW_TABLE
from src.trace_processor.tables.metadata_tables import PROCESS_TABLE
from src.trace_processor.tables.profiler_tables import STACK_PROFILE_CALLSITE_TABLE
from src.trace_processor.tables.slice_tables import SLICE_TABLE
from src.trace_processor.tables.sched_tables import SCHED_SLICE_TABLE

ANCESTOR_SLICE_TABLE = Table(
    python_module=__file__,
    class_name="AncestorSliceTable",
    sql_name="ancestor_slice",
    columns=[
        C("start_id", CppTableId(SLICE_TABLE), flags=ColumnFlag.HIDDEN),
    ],
    parent=SLICE_TABLE)

ANCESTOR_SLICE_BY_STACK_TABLE = Table(
    python_module=__file__,
    class_name="AncestorSliceByStackTable",
    sql_name="ancestor_slice_by_stack",
    columns=[
        C("start_stack_id", CppInt64(), flags=ColumnFlag.HIDDEN),
    ],
    parent=SLICE_TABLE)

ANCESTOR_STACK_PROFILE_CALLSITE_TABLE = Table(
    python_module=__file__,
    class_name="AncestorStackProfileCallsiteTable",
    sql_name="experimental_ancestor_stack_profile_callsite",
    columns=[
        C("start_id",
          CppTableId(STACK_PROFILE_CALLSITE_TABLE),
          flags=ColumnFlag.HIDDEN),
    ],
    parent=STACK_PROFILE_CALLSITE_TABLE)

CONNECTED_FLOW_TABLE = Table(
    python_module=__file__,
    class_name="ConnectedFlowTable",
    sql_name="not_exposed_to_sql",
    columns=[
        C("start_id", CppTableId(SLICE_TABLE), flags=ColumnFlag.HIDDEN),
    ],
    parent=FLOW_TABLE)

DESCENDANT_SLICE_TABLE = Table(
    python_module=__file__,
    class_name="DescendantSliceTable",
    sql_name="descendant_slice",
    columns=[
        C("start_id", CppTableId(SLICE_TABLE), flags=ColumnFlag.HIDDEN),
    ],
    parent=SLICE_TABLE)

DESCENDANT_SLICE_BY_STACK_TABLE = Table(
    python_module=__file__,
    class_name="DescendantSliceByStackTable",
    sql_name="descendant_slice_by_stack",
    columns=[
        C("start_stack_id", CppInt64(), flags=ColumnFlag.HIDDEN),
    ],
    parent=SLICE_TABLE)

EXPERIMENTAL_ANNOTATED_CALLSTACK_TABLE = Table(
    python_module=__file__,
    class_name="ExperimentalAnnotatedCallstackTable",
    sql_name="experimental_annotated_callstack",
    columns=[
        C("annotation", CppString()),
        C("start_id",
          CppTableId(STACK_PROFILE_CALLSITE_TABLE),
          flags=ColumnFlag.HIDDEN),
    ],
    parent=STACK_PROFILE_CALLSITE_TABLE)

EXPERIMENTAL_ANNOTATED_CALLSTACK_TABLE = Table(
    python_module=__file__,
    class_name="ExperimentalAnnotatedCallstackTable",
    sql_name="experimental_annotated_callstack",
    columns=[
        C("annotation", CppString()),
        C("start_id",
          CppTableId(STACK_PROFILE_CALLSITE_TABLE),
          flags=ColumnFlag.HIDDEN),
    ],
    parent=STACK_PROFILE_CALLSITE_TABLE)

EXPERIMENTAL_ANNOTATED_CALLSTACK_TABLE = Table(
    python_module=__file__,
    class_name="ExperimentalAnnotatedCallstackTable",
    sql_name="experimental_annotated_callstack",
    columns=[
        C("annotation", CppString()),
        C("start_id",
          CppTableId(STACK_PROFILE_CALLSITE_TABLE),
          flags=ColumnFlag.HIDDEN),
    ],
    parent=STACK_PROFILE_CALLSITE_TABLE)

EXPERIMENTAL_COUNTER_DUR_TABLE = Table(
    python_module=__file__,
    class_name="ExperimentalCounterDurTable",
    sql_name="experimental_counter_dur",
    columns=[
        C("dur", CppInt64()),
        C("delta", CppDouble()),
    ],
    parent=COUNTER_TABLE)

EXPERIMENTAL_SCHED_UPID_TABLE = Table(
    python_module=__file__,
    class_name="ExperimentalSchedUpidTable",
    sql_name="experimental_sched_upid",
    columns=[
        C("upid", CppOptional(CppTableId(PROCESS_TABLE))),
    ],
    parent=SCHED_SLICE_TABLE)

EXPERIMENTAL_SLICE_LAYOUT_TABLE = Table(
    python_module=__file__,
    class_name="ExperimentalSliceLayoutTable",
    sql_name="experimental_slice_layout",
    columns=[
        C("layout_depth", CppUint32()),
        C("filter_track_ids", CppString(), flags=ColumnFlag.HIDDEN),
    ],
    parent=SLICE_TABLE)

# Keep this list sorted.
ALL_TABLES = [
    ANCESTOR_SLICE_BY_STACK_TABLE,
    ANCESTOR_SLICE_TABLE,
    ANCESTOR_STACK_PROFILE_CALLSITE_TABLE,
    CONNECTED_FLOW_TABLE,
    DESCENDANT_SLICE_BY_STACK_TABLE,
    DESCENDANT_SLICE_TABLE,
    EXPERIMENTAL_ANNOTATED_CALLSTACK_TABLE,
    EXPERIMENTAL_COUNTER_DUR_TABLE,
    EXPERIMENTAL_SCHED_UPID_TABLE,
    EXPERIMENTAL_SLICE_LAYOUT_TABLE,
]
