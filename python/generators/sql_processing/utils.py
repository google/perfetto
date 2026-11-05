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

from enum import Enum
from typing import List


def is_internal(name: str) -> bool:
  """Check if a name represents an internal artifact (starts with _)."""
  return name.startswith('_')


PKG_COMMON = "common"
PKG_VIZ = "viz"
PKG_CHROME = "chrome"
PKG_ANDROID = "android"

ALLOWED_PREFIXES = {
    'android': ['heap_graph', 'memory'],
    'counters': ['counter'],
    'chrome/util': ['cr'],
    'intervals': ['interval'],
    'graphs': ['graph'],
    'slices': [
        'slice', 'thread_slice', 'process_slice', 'thread_or_process_slice'
    ],
    'linux': ['cpu', 'memory'],
    'stacks': ['cpu_profiling'],
    'std/gpu': ['gpu'],
    'std/metasql': ['metasql'],
}

# Allows for nonstandard object names.
OBJECT_NAME_ALLOWLIST = {
    'graphs/partition.sql': ['tree_structural_partition_by_group'],
    'slices/stack.sql': [
        'ancestor_slice_by_stack', 'descendant_slice_by_stack'
    ],
}

# Tables that are allowed to use CREATE TABLE instead of CREATE PERFETTO TABLE
CREATE_TABLE_ALLOWLIST = {'_trace_bounds', '_trace_metrics'}

COLUMN_TYPES = [
    # Standard types
    'LONG',
    'DOUBLE',
    'STRING',
    'BOOL',
    'BYTES',
    'ANY',

    # Special types
    'TIMESTAMP',
    'DURATION',
    'ID',
    'JOINID',
    'ARGSETID'
]

MACRO_ARG_TYPES = [
    'TABLEORSUBQUERY',
    'EXPR',
    'EXPRLIST',
    'UNPARENEXPRLIST',
    'COLUMNNAME',
    'COLUMNNAMELIST',
    # Internal macro arg types for advanced use cases
    '_PROJECTIONFRAGMENT',  # Fragment of a SELECT projection list
    '_TABLENAMELIST'  # List of table names
]


class ObjKind(str, Enum):
  table_view = 'table_view'
  function = 'function'
  table_function = 'table_function'
  macro = 'macro'


# Validates a module from `pfsql parse` for banned statements and words, valid
# includes and deprecated patterns.
def check_banned_usage(parsed, module: dict) -> List[str]:
  errors = _check_include_rules(parsed)
  references = {r.casefold() for r in module['references']}
  for symbol in module['symbols']:
    references.update(r.casefold() for r in symbol['references'])
    name = symbol['name']
    if symbol['form'] != 'plain':
      continue
    if symbol['kind'] == 'view':
      errors.append(f"CREATE VIEW '{name}' is deprecated. "
                    "Use CREATE PERFETTO VIEW instead.")
    elif name not in CREATE_TABLE_ALLOWLIST:
      errors.append(
          f"Table '{name}' uses CREATE TABLE which is deprecated "
          "and this table is not allowlisted. Use CREATE PERFETTO TABLE.")

  if 'run_metric' in references:
    errors.append("RUN_METRIC is banned in standard library.")
  if 'create_function' in references:
    errors.append('CREATE_FUNCTION is deprecated in trace processor. '
                  'Use CREATE PERFETTO FUNCTION instead.')
  if 'create_view_function' in references:
    errors.append('CREATE_VIEW_FUNCTION is deprecated in trace processor. '
                  'Use CREATE PERFETTO FUNCTION $name RETURNS TABLE instead.')
  if 'import' in references:
    errors.append('SELECT IMPORT is deprecated in trace processor. '
                  'Use INCLUDE PERFETTO MODULE instead.')
  if module['uses_like']:
    errors.append('LIKE is banned in trace processor metrics and stdlib. '
                  'Prefer GLOB instead.\n')
  for mutation in module['mutations']:
    if mutation['kind'] == 'insert':
      errors.append("INSERT INTO table is not allowed in standard library.")
    else:
      errors.append(f"Dropping object {mutation['object_type'].upper()} "
                    f"'{mutation['name']}' is banned.")
  if any(include.endswith('*') for include in module['includes']):
    errors.append(
        "INCLUDE PERFETTO MODULE with wildcards is not allowed in stdlib. "
        "Import specific modules instead.")
  return errors + parsed.errors


def _check_include_rules(parsed) -> List[str]:
  errors = []
  package = parsed.package_name.lower() if parsed.package_name else ''
  for include in parsed.includes:
    include_package = include.package.lower() if include.package else ''

    if include_package == PKG_COMMON:
      errors.append(
          "Common module has been deprecated in the standard library. "
          "Please check `slices.with_context` for a replacement for "
          "`common.slices` and `time.conversion` for replacement for "
          "`common.timestamps`")

    if package != PKG_VIZ and include_package == PKG_VIZ:
      errors.append(
          f"No modules can depend on '{PKG_VIZ}' outside '{PKG_VIZ}' package.")

    if package == PKG_CHROME and include_package == PKG_ANDROID:
      errors.append(
          f"Modules from package '{PKG_CHROME}' can't include '{include.module}' "
          f"from package '{PKG_ANDROID}'")

    if package == PKG_ANDROID and include_package == PKG_CHROME:
      errors.append(
          f"Modules from package '{PKG_ANDROID}' can't include '{include.module}' "
          f"from package '{PKG_CHROME}'")
  return errors
