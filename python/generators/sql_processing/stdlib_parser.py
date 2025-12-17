#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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
Unified stdlib parser library for Perfetto SQL standard library.

This module provides functions to parse stdlib SQL files and generate
structured output for consumption by various tools.
"""

import os
from collections import defaultdict
from pathlib import Path
from typing import List, Tuple, Optional

from python.generators.sql_processing.docs_parse import DocParseOptions, ParsedModule, parse_file
from python.generators.sql_processing.utils import is_internal
from python.generators.sql_processing.stdlib_tags import get_tags, get_table_importance
from python.perfetto.trace_data_checks import MODULE_DATA_CHECK_SQL

ROOT_DIR = os.path.dirname(
    os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def find_stdlib_path():
  """Find the stdlib directory in the current repository."""
  stdlib_path = os.path.join(ROOT_DIR, "src", "trace_processor", "perfetto_sql",
                             "stdlib")

  if not os.path.exists(stdlib_path):
    raise ValueError(f"stdlib path not found: {stdlib_path}")

  return Path(stdlib_path)


def get_module_name(rel_path: str) -> str:
  """Convert a relative SQL file path to its module name.

  Args:
    rel_path: Relative path from stdlib root (e.g., "slices/stack.sql")

  Returns:
    Module name (e.g., "slices.stack")
  """
  # Remove .sql extension
  path_without_ext = rel_path.removesuffix('.sql')
  # Convert path separators to dots for module name
  module_name = path_without_ext.replace(os.sep, '.')
  return module_name


def parse_all_modules(
    stdlib_path: str,
    include_internal: bool = False,
    name_filter: Optional[str] = None
) -> List[Tuple[str, str, str, ParsedModule]]:
  """Parse all SQL modules in the stdlib.

  Args:
    stdlib_path: Path to stdlib directory
    include_internal: Whether to include internal (private) artifacts
    name_filter: Optional regex to filter module names

  Returns:
    List of tuples: (abs_path, rel_path, module_name, parsed_module)
  """
  import re

  modules = []
  for root, _, files in os.walk(stdlib_path, topdown=True):
    for f in files:
      abs_path = os.path.join(root, f)
      if not abs_path.endswith(".sql"):
        continue

      rel_path = os.path.relpath(abs_path, stdlib_path)
      module_name = get_module_name(rel_path)

      # Apply name filter if provided
      if name_filter is not None:
        try:
          pattern = re.compile(name_filter)
        except re.error as e:
          raise ValueError(f"Invalid regex pattern '{name_filter}': {e}")
        if not pattern.match(rel_path):
          continue

      # Read and parse the file
      with open(abs_path, 'r', encoding='utf-8') as f:
        sql = f.read()

      parsed = parse_file(
          rel_path,
          sql,
          options=DocParseOptions(
              enforce_every_column_set_is_documented=True,
              include_internal=include_internal),
      )

      # Some modules (i.e. `deprecated`) should not generate output
      if not parsed:
        continue

      modules.append((abs_path, rel_path, module_name, parsed))

  return modules


def format_entities(modules: List[Tuple[str, str, str, ParsedModule]]) -> dict:
  """Format parsed modules as entity map for dependency checking.

  Output format:
  {
    "modules": {
      "slices.stack": {
        "entities": [
          {"name": "stack_from_stack_profile_callsite", "is_internal": false},
          {"name": "_intervals_flatten", "is_internal": true}
        ],
        "includes": ["slices.with_context", "graphs.search"]
      },
      ...
    },
    "entity_to_module": {
      "stack_from_stack_profile_callsite": "slices.stack",
      "_intervals_flatten": "slices.stack",
      ...
    }
  }
  """

  modules_dict = {}
  entity_to_module = {}

  for _, _, module_name, parsed in modules:
    # Extract all entity names with internal flag
    entities = []

    # Tables and views
    for table in parsed.table_views:
      entities.append({
          "name": table.name,
          "is_internal": is_internal(table.name)
      })
      entity_to_module[table.name] = module_name

    # Functions
    for func in parsed.functions:
      entities.append({
          "name": func.name,
          "is_internal": is_internal(func.name)
      })
      entity_to_module[func.name] = module_name

    # Table functions
    for func in parsed.table_functions:
      entities.append({
          "name": func.name,
          "is_internal": is_internal(func.name)
      })
      entity_to_module[func.name] = module_name

    # Macros
    for macro in parsed.macros:
      entities.append({
          "name": macro.name,
          "is_internal": is_internal(macro.name)
      })
      entity_to_module[macro.name] = module_name

    # Extract includes
    # Note: inc.module already contains the full module name
    # Example: inc.module = "android.suspend", inc.package = "android"
    includes = [inc.module for inc in parsed.includes]

    modules_dict[module_name] = {
        "entities": entities,
        "includes": includes,
    }

  return {
      "modules": modules_dict,
      "entity_to_module": entity_to_module,
  }


def format_docs(modules: List[Tuple[str, str, str, ParsedModule]]) -> list:
  """Format parsed modules as documentation JSON (for gen_stdlib_docs_json).

  Output format matches what gen_stdlib_docs_json currently produces.
  """

  # Use the curated data check SQL map
  data_check_sql_map = MODULE_DATA_CHECK_SQL

  def _summary_desc(s: str) -> str:
    """Extract the first sentence from a description."""
    s = s.replace('\n', ' ')
    if '. ' in s:
      return s.split('. ')[0]
    elif '.' in s:
      return s.split('.')[0]
    return s

  def _create_field_dict(name: str, obj, include_desc: bool = True) -> dict:
    """Create a dictionary for a column or argument.

    Parses long_type to extract table and column references.
    Expected format: "TYPE(table_name.column_name)" where TYPE is optional uppercase,
    and table_name and column_name are lowercase with underscores.
    If the format doesn't match, table and column are set to None.
    """
    import re

    # Parse long type string to extract table and column references
    # Expected format: "TYPE(table_name.column_name)"
    table, column = None, None
    if hasattr(obj, 'long_type') and obj.long_type:
      pattern = r'[A-Z]*\(([a-z_]*)\.([a-z_]*)\)'
      m = re.match(pattern, obj.long_type)
      if m:
        table, column = m.groups()

    result = {
        'name': name,
        'type': obj.long_type if hasattr(obj, 'long_type') else None,
        'table': table,
        'column': column,
    }
    if include_desc:
      result['desc'] = obj.description if hasattr(obj, 'description') else None
    return result

  packages = defaultdict(list)

  for _, _, module_name, parsed in modules:
    package_name = module_name.split(".")[0]

    module_dict = {
        'module_name': module_name,
        'module_doc': {
            'name': parsed.module_doc.name,
            'desc': parsed.module_doc.desc,
        } if parsed.module_doc else None,
        'tags': get_tags(module_name),
        'includes': [inc.module for inc in parsed.includes],
        'data_objects': [{
            'name':
                table.name,
            'desc':
                table.desc,
            'summary_desc':
                _summary_desc(table.desc),
            'type':
                table.type,
            'visibility':
                'private' if is_internal(table.name) else 'public',
            'importance':
                get_table_importance(table.name),
            'cols': [
                _create_field_dict(col_name, col)
                for (col_name, col) in table.cols.items()
            ]
        }
                         for table in parsed.table_views],
        'functions': [{
            'name': function.name,
            'desc': function.desc,
            'summary_desc': _summary_desc(function.desc),
            'visibility': 'private' if is_internal(function.name) else 'public',
            'args': [
                _create_field_dict(arg_name, arg)
                for (arg_name, arg) in function.args.items()
            ],
            'return_type': function.return_type,
            'return_desc': function.return_desc,
        }
                      for function in parsed.functions],
        'table_functions': [{
            'name':
                function.name,
            'desc':
                function.desc,
            'summary_desc':
                _summary_desc(function.desc),
            'visibility':
                'private' if is_internal(function.name) else 'public',
            'args': [
                _create_field_dict(arg_name, arg)
                for (arg_name, arg) in function.args.items()
            ],
            'cols': [
                _create_field_dict(col_name, col)
                for (col_name, col) in function.cols.items()
            ]
        }
                            for function in parsed.table_functions],
        'macros': [{
            'name':
                macro.name,
            'desc':
                macro.desc,
            'summary_desc':
                _summary_desc(macro.desc),
            'visibility':
                'private' if is_internal(macro.name) else 'public',
            'return_desc':
                macro.return_desc,
            'return_type':
                macro.return_type,
            'args': [
                _create_field_dict(arg_name, arg)
                for (arg_name, arg) in macro.args.items()
            ],
        }
                   for macro in parsed.macros],
        'data_check_sql': data_check_sql_map.get(module_name),
    }
    packages[package_name].append(module_dict)

  packages_list = [{
      "name": name,
      "modules": modules
  } for name, modules in packages.items()]

  return packages_list


def format_full(modules: List[Tuple[str, str, str, ParsedModule]]) -> dict:
  """Format parsed modules with full information (for check_sql_modules.py).

  Includes raw SQL and parsed module data for validation.
  """
  modules_list = []

  for abs_path, rel_path, module_name, parsed in modules:
    # Read raw SQL
    with open(abs_path, 'r', encoding='utf-8') as f:
      sql = f.read()

    # Extract includes in the format needed
    includes = [{
        'package':
            inc.package,
        'module':
            inc.module,
        'full_name':
            f"{inc.package}.{inc.module}" if inc.package else inc.module
    } for inc in parsed.includes]

    module_dict = {
        'path': abs_path,
        'rel_path': rel_path,
        'module_name': module_name,
        'package_name': parsed.package_name,
        'sql': sql,
        'includes': includes,
        'errors': parsed.errors,
        'functions_count': len(parsed.functions),
        'table_functions_count': len(parsed.table_functions),
        'table_views_count': len(parsed.table_views),
        'macros_count': len(parsed.macros),
    }
    modules_list.append(module_dict)

  return {'modules': modules_list}
