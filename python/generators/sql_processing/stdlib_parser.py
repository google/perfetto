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
from typing import List, Tuple, Optional

from python.generators.sql_processing.docs_parse import DocParseOptions, ParsedModule, parse_file
from python.generators.sql_processing.utils import is_internal


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
