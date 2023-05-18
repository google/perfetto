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

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppUint32

ROOT_TABLE = Table(
    python_module=__file__,
    class_name="RootTestTable",
    sql_name="root_table",
    columns=[
        C("root_sorted", CppUint32(), flags=ColumnFlag.SORTED),
        C("root_non_null", CppUint32()),
        C("root_non_null_2", CppUint32()),
        C("root_nullable", CppOptional(CppUint32())),
    ])

CHILD_TABLE = Table(
    python_module=__file__,
    class_name="ChildTestTable",
    sql_name="child_table",
    parent=ROOT_TABLE,
    columns=[
        C("child_sorted", CppUint32(), flags=ColumnFlag.SORTED),
        C("child_non_null", CppUint32()),
        C("child_nullable", CppOptional(CppUint32())),
    ])

# Keep this list sorted.
ALL_TABLES = [
    ROOT_TABLE,
    CHILD_TABLE,
]
