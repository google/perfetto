# Copyright (C) 2024 The Android Open Source Project
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
"""
Contains file-related tables. ETM tables have been moved to
src/trace_processor/plugins/etm_tables.py and are owned by EtmTpPlugin.
"""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc

FILE_TABLE = Table(
    python_module=__file__,
    class_name='FileTable',
    sql_name='__intrinsic_file',
    columns=[
        C('name', CppString()),
        C(
            'size',
            CppInt64(),
            cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE,
        ),
        C(
            'trace_type',
            CppString(),
            cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE,
        ),
    ],
    tabledoc=TableDoc(
        doc='''
            Metadata related to the trace file parsed. Note the order in which
            the files appear in this table corresponds to the order in which
            they are read and sent to the tokenization stage.
        ''',
        group='Misc',
        columns={
            'parent_id':
                '''
                  Parent file. E.g. files contained in a zip file will point to
                  the zip file.
                ''',
            'name':
                '''File name, if known, NULL otherwise''',
            'size':
                '''Size in bytes''',
            'trace_type':
                '''Trace type''',
            'processing_order':
                '''In which order where the files were processed.''',
        }))

ELF_FILE_TABLE = Table(
    python_module=__file__,
    class_name='ElfFileTable',
    sql_name='__intrinsic_elf_file',
    columns=[
        C('file_id', CppTableId(FILE_TABLE), cpp_access=CppAccess.READ),
        C('load_bias', CppInt64()),
        C('build_id', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
            Metadata related to the trace file parsed. Note the order in which
            the files appear in this table corresponds to the order in which
            they are read and sent to the tokenization stage.
        ''',
        group='Misc',
        columns={
            'parent_id':
                '''
                  Parent file. E.g. files contained in a zip file will point to
                  the zip file.
                ''',
            'name':
                '''File name, if known, NULL otherwise''',
            'size':
                '''Size in bytes''',
            'trace_type':
                '''Trace type''',
            'processing_order':
                '''In which order where the files were processed.''',
        }))

ALL_TABLES = [FILE_TABLE, ELF_FILE_TABLE]
