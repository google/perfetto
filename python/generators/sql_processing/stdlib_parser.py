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
import re
from typing import List, Tuple, Optional

from python.generators.sql_processing.docs_parse import DocParseOptions, ParsedModule, parse_module
from python.generators.sql_processing.pfsql import parse_modules


def parse_all_modules(
    stdlib_path: str,
    pfsql_binary: str,
    include_internal: bool = False,
    name_filter: Optional[str] = None,
    pfsql_modules: Optional[List[dict]] = None,
) -> List[Tuple[str, str, str, ParsedModule]]:
  """Parses all SQL modules in the stdlib with pfsql.

  Args:
    stdlib_path: Path to stdlib directory
    pfsql_binary: Path to a pfsql binary
    include_internal: Whether to include internal (private) artifacts
    name_filter: Optional regex to filter module names
    pfsql_modules: Output of parse_modules(), if already available

  Returns:
    List of tuples: (abs_path, rel_path, module_name, parsed_module)
  """
  pattern = re.compile(name_filter) if name_filter else None
  options = DocParseOptions(
      enforce_every_column_set_is_documented=True,
      include_internal=include_internal)

  modules = []
  for module in pfsql_modules or parse_modules(stdlib_path, pfsql_binary):
    rel_path = module['path']
    if pattern and not pattern.match(rel_path):
      continue
    parsed = parse_module(rel_path, module, options)
    if parsed:
      abs_path = os.path.join(stdlib_path, rel_path)
      modules.append((abs_path, rel_path, module['module'], parsed))
  return modules
