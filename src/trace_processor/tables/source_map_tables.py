# Copyright (C) 2025 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http:#www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Copyright 2025 The Lynx Authors. All rights reserved.
# Licensed under the Apache License Version 2.0 that can be found in the
# LICENSE file in the root directory of this source tree.
"""Contains tables for relevant for TODO."""

from python.generators.trace_processor_table.public import Column as C, WrappingSqlView
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

SOURCE_FILE_TABLE = Table(
    python_module=__file__,
    class_name='SourceFileTable',
    sql_name='__internal_source_file',
    columns=[
        C('file', CppString()),
        C('content', CppString()),
    ],
    wrapping_sql_view=WrappingSqlView('source_file'),
    tabledoc=TableDoc(
        doc='''
          JS Profile Source File table
        ''',
        group='Proto',
        columns={
            'file': 'file name',
            'content': 'content',
        }))

# Keep this list sorted.
ALL_TABLES = [
    SOURCE_FILE_TABLE,
]
