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
"""Contains tables for relevant for TODO."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32

from src.trace_processor.tables.slice_tables import SLICE_TABLE

FLOW_TABLE = Table(
    python_module=__file__,
    class_name='FlowTable',
    sql_name='flow',
    columns=[
        C('slice_out', CppTableId(SLICE_TABLE)),
        C('slice_in', CppTableId(SLICE_TABLE)),
        C('arg_set_id', CppUint32()),
    ],
    tabledoc=TableDoc(
        doc='''''',
        group='Misc',
        columns={
            'arg_set_id': '''''',
            'slice_out': '''''',
            'slice_in': ''''''
        }))

# Keep this list sorted.
ALL_TABLES = [
    FLOW_TABLE,
]
