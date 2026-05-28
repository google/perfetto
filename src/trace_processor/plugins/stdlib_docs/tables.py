# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import Table

STDLIB_DOCS_MODULES_TABLE = Table(
    python_module=__file__,
    class_name="StdlibDocsModulesTable",
    sql_name="__intrinsic_stdlib_modules",
    columns=[
        C("module", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("package", CppString(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
    ],
)

STDLIB_DOCS_TABLES_TABLE = Table(
    python_module=__file__,
    class_name="StdlibDocsTablesTable",
    sql_name="not_exposed_to_sql",
    columns=[
        C("name", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("type", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("description",
          CppString(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("exposed", CppInt64(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("cols", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
    ],
)

STDLIB_DOCS_FUNCTIONS_TABLE = Table(
    python_module=__file__,
    class_name="StdlibDocsFunctionsTable",
    sql_name="not_exposed_to_sql",
    columns=[
        C("name", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("description",
          CppString(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("exposed", CppInt64(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("is_table_function",
          CppInt64(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("return_type",
          CppString(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("return_description",
          CppString(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("args", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("cols", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
    ],
)

STDLIB_DOCS_MACROS_TABLE = Table(
    python_module=__file__,
    class_name="StdlibDocsMacrosTable",
    sql_name="not_exposed_to_sql",
    columns=[
        C("name", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("description",
          CppString(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("exposed", CppInt64(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("return_type",
          CppString(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("return_description",
          CppString(),
          cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
        C("args", CppString(), cpp_access=CppAccess.READ_AND_HIGH_PERF_WRITE),
    ],
)

# Keep this list sorted.
ALL_TABLES = [
    STDLIB_DOCS_FUNCTIONS_TABLE,
    STDLIB_DOCS_MACROS_TABLE,
    STDLIB_DOCS_MODULES_TABLE,
    STDLIB_DOCS_TABLES_TABLE,
]
