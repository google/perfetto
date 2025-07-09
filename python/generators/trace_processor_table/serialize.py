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

from typing import List, Union
from typing import Optional

from python.generators.trace_processor_table.public import Alias
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.util import ParsedTable
from python.generators.trace_processor_table.util import ParsedColumn
from python.generators.trace_processor_table.util import data_layer_type
from python.generators.trace_processor_table.util import parse_type
from python.generators.trace_processor_table.util import typed_column_type
from python.generators.trace_processor_table.serialize_new import TableSerializer as NewTableSerializer


def serialize_header(ifdef_guard: str, tables: List[ParsedTable],
                     include_paths: List[str]) -> str:
  """Serializes a table header file containing the given set of tables."""
  # Replace the backslash with forward slash when building on Windows.
  # Caused b/327985369 without the replace.
  include_paths_str = '\n'.join([f'#include "{i}"' for i in include_paths
                                ]).replace("\\", "/")
  serializers: List[NewTableSerializer] = []
  for t in tables:
    serializers.append(NewTableSerializer(t))
  tables_str = '\n\n'.join([t.serialize() for t in serializers])
  return f'''
#ifndef {ifdef_guard}
#define {ifdef_guard}

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/typed_cursor.h"
#include "src/trace_processor/tables/macros_internal.h"

{include_paths_str}

namespace perfetto::trace_processor::tables {{

{tables_str.strip()}

}}  // namespace perfetto

#endif  // {ifdef_guard}
  '''.strip()


def to_cpp_flags(raw_flag: ColumnFlag) -> str:
  """Converts a ColumnFlag to the C++ flags which it represents

  It is not valid to call this function with ColumnFlag.NONE as in this case
  defaults for that column should be implicitly used."""

  assert raw_flag != ColumnFlag.NONE
  flags = []
  if ColumnFlag.SORTED in raw_flag:
    flags.append('ColumnLegacy::Flag::kSorted')
  if ColumnFlag.HIDDEN in raw_flag:
    flags.append('ColumnLegacy::Flag::kHidden')
  if ColumnFlag.DENSE in raw_flag:
    flags.append('ColumnLegacy::Flag::kDense')
  if ColumnFlag.SET_ID in raw_flag:
    flags.append('ColumnLegacy::Flag::kSetId')
  return ' | '.join(flags)
