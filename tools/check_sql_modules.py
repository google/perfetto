#!/usr/bin/env python3
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
"""
Check Perfetto stdlib modules for banned patterns and documentation issues.

This tool validates Perfetto SQL standard library modules for:
- Banned SQL patterns (CREATE TABLE AS, CREATE VIEW AS, DROP statements, etc.)
- Documentation completeness for public artifacts
- Proper dependency declarations via INCLUDE PERFETTO MODULE (with --check-includes)
"""

import argparse
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import List, Set, Dict, Tuple

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.stdlib_parser import parse_all_modules, format_entities
from python.generators.sql_processing.utils import check_banned_create_table_as
from python.generators.sql_processing.utils import check_banned_create_view_as
from python.generators.sql_processing.utils import check_banned_words
from python.generators.sql_processing.utils import check_banned_drop
from python.generators.sql_processing.utils import check_banned_include_all
from python.generators.sql_processing.utils import is_internal
from python.generators.sql_processing.stdlib_tags import MODULE_TAGS, VALID_TAGS

# Package name constants
PKG_COMMON = "common"
PKG_VIZ = "viz"
PKG_CHROME = "chrome"
PKG_ANDROID = "android"
PKG_PRELUDE = "prelude"


@dataclass
class ModuleInfo:
  """Information about a module for include checking."""
  name: str
  includes: List[str] = field(default_factory=list)
  entities: List[Dict] = field(default_factory=list)
  prelude_imports: List[str] = field(default_factory=list)
  silent_imports: List[str] = field(default_factory=list)
  unused_imports: List[str] = field(default_factory=list)
  used_entities_by_include: Dict[str, Set[str]] = field(default_factory=dict)


def extract_referenced_entities(sql_file: str) -> Set[str]:
  """Extract all entity references from a SQL file.

  This uses regex-based parsing which has limitations:
  - May miss some references in complex SQL
  - May produce false positives in edge cases
  - Does not handle all SQL comment styles perfectly

  Returns:
    Set of entity names referenced in the file, or empty set on error.
  """
  # Common SQL built-in functions that should be filtered out
  SQL_BUILTINS = {
      'COUNT', 'SUM', 'AVG', 'MAX', 'MIN', 'ABS', 'CAST', 'COALESCE', 'IFNULL',
      'NULLIF', 'SUBSTR', 'LENGTH', 'UPPER', 'LOWER', 'TRIM', 'REPLACE',
      'ROUND', 'FLOOR', 'CEIL', 'DATETIME', 'DATE', 'TIME', 'STRFTIME',
      'JULIANDAY', 'LAG', 'LEAD', 'ROW_NUMBER', 'RANK', 'DENSE_RANK', 'NTILE',
      'FIRST_VALUE', 'LAST_VALUE', 'NTH_VALUE', 'GROUP_CONCAT', 'JSON_EXTRACT',
      'JSON_ARRAY', 'JSON_OBJECT', 'PRINTF', 'CHAR', 'UNICODE', 'QUOTE',
      'RANDOMBLOB', 'ZEROBLOB', 'HEX', 'UNHEX', 'TYPEOF', 'SQLITE_VERSION',
      'TOTAL_CHANGES', 'CHANGES', 'LAST_INSERT_ROWID'
  }

  # Perfetto SQL type names and common SQL keywords to filter out
  SQL_TYPES_AND_KEYWORDS = {
      # Perfetto SQL types
      'TABLEORSUBQUERY',
      'COLUMNNAME',
      'EXPR',
      'STRING',
      'LONG',
      'INT',
      'BOOL',
      'DOUBLE',
      'TIMESTAMP',
      'DURATION',
      'ARGSETID',
      'ID',
      'JOINID',
      # SQL keywords
      'SELECT',
      'FROM',
      'WHERE',
      'JOIN',
      'LEFT',
      'RIGHT',
      'INNER',
      'OUTER',
      'ON',
      'USING',
      'GROUP',
      'BY',
      'ORDER',
      'HAVING',
      'LIMIT',
      'OFFSET',
      'AS',
      'ASC',
      'DESC',
      'AND',
      'OR',
      'NOT',
      'IN',
      'IS',
      'NULL',
      'TRUE',
      'FALSE',
      'CASE',
      'WHEN',
      'THEN',
      'ELSE',
      'END',
      'WITH',
      'PARTITION',
      'PARTITIONED',
      'OVER',
      'RETURNS',
      'CREATE',
      'DROP',
      'INSERT',
      'UPDATE',
      'DELETE',
      'CROSS'
  }

  references = set()

  try:
    with open(sql_file, 'r', encoding='utf-8') as f:
      content = f.read()

    # Remove both single-line (--) and multi-line (/* */) comments
    # First, remove multi-line comments
    content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)

    # Then remove single-line comments while preserving string literals
    lines = []
    for line in content.split('\n'):
      comment_pos = -1
      in_string = False
      string_char = None
      for i, char in enumerate(line):
        if char in ('"', "'") and (i == 0 or line[i - 1] != '\\'):
          if not in_string:
            in_string = True
            string_char = char
          elif char == string_char:
            in_string = False
            string_char = None
        elif char == '-' and i < len(line) - 1 and line[
            i + 1] == '-' and not in_string:
          comment_pos = i
          break
      if comment_pos >= 0:
        line = line[:comment_pos]
      lines.append(line)
    content = '\n'.join(lines)

    # Extract entity references (tables, views, functions, macros)
    # Look for identifiers that are likely entities

    # Handle SPAN_JOIN and SPAN_OUTER_JOIN specially (extracts multiple tables)
    span_join_pattern = r'\bSPAN_(?:OUTER_)?JOIN\s*\(([^)]+)\)'
    for match in re.finditer(span_join_pattern, content, re.IGNORECASE):
      span_content = match.group(1)
      # Extract table names (identifiers before PARTITIONED, before comma, or at end)
      # Pattern: identifier followed by PARTITIONED, comma, or end of content
      table_pattern = r'\b([a-zA-Z_][a-zA-Z0-9_]*)\s*(?:PARTITIONED|,|$)'
      for table_match in re.finditer(table_pattern, span_content,
                                     re.IGNORECASE):
        entity = table_match.group(1)
        # Filter out keywords like PARTITIONED itself
        if entity.upper() not in SQL_BUILTINS and entity.upper(
        ) != 'PARTITIONED':
          references.add(entity)

    # Other patterns
    patterns = [
        r'\bFROM\s+([a-zA-Z_][a-zA-Z0-9_]*)\b',
        r'\bJOIN\s+([a-zA-Z_][a-zA-Z0-9_]*)\b',
        r'\bIN\s+([a-zA-Z_][a-zA-Z0-9_]*)\b',  # IN table_name or NOT IN table_name
        r'\b([a-zA-Z_][a-zA-Z0-9_!]*)\s*\(',
    ]

    for pattern in patterns:
      matches = re.finditer(pattern, content, re.IGNORECASE)
      for match in matches:
        entity = match.group(1)
        # Filter out SQL built-in functions
        if entity.upper() not in SQL_BUILTINS:
          # Strip trailing '!' from macro invocations (macros are defined without '!' but invoked with it)
          entity_normalized = entity.rstrip('!')
          references.add(entity_normalized)

    # Extract table arguments from macro calls
    # Pattern: macro_name!(arg1, arg2, ...) where args could be table references
    macro_call_pattern = r'\b([a-zA-Z_][a-zA-Z0-9_]*!)\s*\('
    for match in re.finditer(macro_call_pattern, content):
      # Find matching closing paren
      start_pos = match.end()
      paren_depth = 1
      pos = start_pos
      while pos < len(content) and paren_depth > 0:
        if content[pos] == '(':
          paren_depth += 1
        elif content[pos] == ')':
          paren_depth -= 1
        pos += 1

      if paren_depth == 0:
        args_str = content[start_pos:pos - 1]
        # Split by commas (simple split, doesn't handle nested parens perfectly)
        # Extract simple identifiers that could be table references
        for arg in args_str.split(','):
          arg = arg.strip()
          # Match simple identifier: not starting with $, not containing dots or SELECT
          if re.match(r'^[a-zA-Z_][a-zA-Z0-9_]*$', arg):
            # Filter out keywords and types
            if (arg.upper() not in SQL_BUILTINS and
                arg.upper() not in SQL_TYPES_AND_KEYWORDS):
              references.add(arg)

  except IOError as e:
    sys.stderr.write(f"Warning: Failed to read {sql_file}: {e}\n")
    return set()
  except Exception as e:
    sys.stderr.write(
        f"Warning: Failed to extract entities from {sql_file}: {e}\n")
    return set()

  return references


def check_includes(modules: List[Tuple], quiet: bool = False) -> int:
  """Check that modules properly declare their dependencies.

  Args:
    modules: List of tuples (abs_path, rel_path, module_name, parsed_module)
    quiet: If True, suppress detailed output

  Returns:
    Number of modules with include errors.
  """
  # Get entity information
  entities_data = format_entities(modules)
  modules_by_name = {}
  entity_to_module = entities_data['entity_to_module']

  # Build modules_by_name from the parsed data
  for module_name, module_data in entities_data['modules'].items():
    module_info = ModuleInfo(
        name=module_name,
        includes=module_data['includes'],
        entities=module_data['entities'])
    modules_by_name[module_name] = module_info

  # Build modules_by_file for reference analysis
  modules_by_file = {}
  for abs_path, _, module_name, _ in modules:
    modules_by_file[abs_path] = modules_by_name.get(module_name)

  # ANALYSIS PASS: Analyze usages and compute dependencies
  for abs_path, module_info in modules_by_file.items():
    if not module_info:
      continue

    # Extract all entity references in this module
    references = extract_referenced_entities(abs_path)

    # Get entities defined in this module (skip self-references)
    # Extract just the names from the entity dicts
    own_entities = {e['name'] for e in module_info.entities}

    # Build map of included module -> entities it provides
    included_entities_by_module = {}
    for included_module_name in module_info.includes:
      if included_module_name in modules_by_name:
        # Extract all entity names (both public and internal)
        # Using internal entities from an included module is allowed
        all_entities = {
            e['name'] for e in modules_by_name[included_module_name].entities
        }
        included_entities_by_module[included_module_name] = all_entities

    # Track which entities from each include are actually used
    used_entities_by_include = defaultdict(set)

    # Track imports that are used but not declared
    prelude_imports = []
    silent_imports = []

    # Analyze each reference
    for ref in references:
      # Skip self-references
      if ref in own_entities:
        continue

      # Check if from an explicitly included module
      is_from_included_module = False
      for included_module_name, entities in included_entities_by_module.items():
        if ref in entities:
          used_entities_by_include[included_module_name].add(ref)
          is_from_included_module = True
          break

      if is_from_included_module:
        continue

      # Find which module defines this entity
      defining_module = entity_to_module.get(ref)
      if not defining_module:
        continue

      # It's an implicit import
      if defining_module.startswith(f'{PKG_PRELUDE}.'):
        prelude_imports.append(f"{ref} (from {defining_module})")
      else:
        silent_imports.append(f"{ref} (from {defining_module})")

    # Find unused imports
    unused_imports = [
        inc for inc in module_info.includes
        if inc not in used_entities_by_include
    ]

    # Store results
    module_info.prelude_imports = sorted(list(set(prelude_imports)))
    module_info.silent_imports = sorted(list(set(silent_imports)))
    module_info.unused_imports = sorted(unused_imports)
    module_info.used_entities_by_include = {
        inc: sorted(list(entities))
        for inc, entities in used_entities_by_include.items()
    }

  # Count modules with errors (excluding prelude modules)
  # Prelude modules are exempt because they provide core functionality
  # that is automatically available to all modules
  modules_with_errors = []
  for module_name in sorted(modules_by_name.keys()):
    module = modules_by_name[module_name]
    if module_name.startswith(f'{PKG_PRELUDE}.'):
      continue
    if module.unused_imports or module.silent_imports:
      modules_with_errors.append(module_name)

  if not quiet:
    if modules_with_errors:
      print(
          f"\nFound {len(modules_with_errors)} module(s) with include errors:\n"
      )
    else:
      print(f"\nNo include errors found in any modules!")
      return 0

  for module_name in sorted(modules_by_name.keys()):
    module = modules_by_name[module_name]

    # Skip prelude modules
    if module_name.startswith(f'{PKG_PRELUDE}.'):
      continue

    # Only show modules with errors
    if not (module.unused_imports or module.silent_imports):
      continue

    print(f"Module: {module.name}")

    if module.unused_imports:
      print(f"  Unused imports ({len(module.unused_imports)}):")
      for imp in sorted(module.unused_imports):
        print(f"    - {imp}")

    if module.silent_imports:
      print(f"  Silent imports ({len(module.silent_imports)}):")
      for imp in sorted(module.silent_imports):
        print(f"    - {imp}")

    print()

  return len(modules_with_errors)


def has_public_artifacts(parsed) -> bool:
  """Check if a parsed module has any public artifacts.

  Args:
    parsed: Parsed module object

  Returns:
    True if the module has any public tables, views, functions, table functions,
    or macros (artifacts not prefixed with '_').
  """
  # Check tables/views
  for table in parsed.table_views:
    if not is_internal(table.name):
      return True

  # Check functions
  for func in parsed.functions:
    if not is_internal(func.name):
      return True

  # Check table functions
  for func in parsed.table_functions:
    if not is_internal(func.name):
      return True

  # Check macros
  for macro in parsed.macros:
    if not is_internal(macro.name):
      return True

  return False


def check_tags(modules: List[Tuple], quiet: bool = False) -> int:
  """Check that all modules with public artifacts have tags defined.

  Args:
    modules: List of tuples (abs_path, rel_path, module_name, parsed_module)
    quiet: If True, suppress detailed output

  Returns:
    Number of modules missing tags.
  """
  modules_missing_tags = []

  for _, _, module_name, parsed in modules:
    # If module has public artifacts, it must have tags
    if has_public_artifacts(parsed):
      tags = MODULE_TAGS.get(module_name, [])
      if not tags:
        modules_missing_tags.append(module_name)

  if not quiet:
    if modules_missing_tags:
      print(
          f"\nFound {len(modules_missing_tags)} module(s) with public artifacts but missing tags:\n"
      )
      for module_name in sorted(modules_missing_tags):
        print(f"  - {module_name}")
      print(
          f"\nPlease add tags for these modules in python/generators/sql_processing/stdlib_tags.py"
      )
    else:
      print(f"\nAll modules with public artifacts have tags defined!")

  return len(modules_missing_tags)


def check_orphaned_tags(modules: List[Tuple], quiet: bool = False) -> int:
  """Check that all tags in MODULE_TAGS correspond to actual modules.

  Args:
    modules: List of tuples (abs_path, rel_path, module_name, parsed_module)
    quiet: If True, suppress detailed output

  Returns:
    Number of orphaned tags (tags for non-existent modules).
  """
  # Build set of actual module names
  actual_modules = set()
  for _, _, module_name, _ in modules:
    actual_modules.add(module_name)

  # Find tags for modules that don't exist
  orphaned_tags = []
  for tagged_module in MODULE_TAGS.keys():
    if tagged_module not in actual_modules:
      orphaned_tags.append(tagged_module)

  if not quiet:
    if orphaned_tags:
      print(f"\nFound {len(orphaned_tags)} tag(s) for non-existent modules:\n")
      for module_name in sorted(orphaned_tags):
        print(f"  - {module_name}")
      print(
          f"\nPlease remove these from python/generators/sql_processing/stdlib_tags.py"
      )
    else:
      print(f"\nNo orphaned tags found!")

  return len(orphaned_tags)


def check_invalid_tags(quiet: bool = False) -> int:
  """Check that all tags used in MODULE_TAGS are from VALID_TAGS.

  Args:
    quiet: If True, suppress detailed output

  Returns:
    Number of invalid tags found.
  """
  invalid_tags_by_module = {}

  # Check each module's tags
  for module_name, tags in MODULE_TAGS.items():
    invalid = []
    for tag in tags:
      if tag not in VALID_TAGS:
        invalid.append(tag)
    if invalid:
      invalid_tags_by_module[module_name] = invalid

  if not quiet:
    if invalid_tags_by_module:
      total_invalid = sum(len(tags) for tags in invalid_tags_by_module.values())
      print(
          f"\nFound {total_invalid} invalid tag(s) in {len(invalid_tags_by_module)} module(s):\n"
      )
      for module_name in sorted(invalid_tags_by_module.keys()):
        print(f"  {module_name}:")
        for tag in sorted(invalid_tags_by_module[module_name]):
          print(f"    - {tag}")
      print(f"\nAll tags must be from VALID_TAGS in stdlib_tags.py")
    else:
      print(f"\nAll tags are valid!")

  return len(invalid_tags_by_module)


def check_nested_tag_parents(quiet: bool = False) -> int:
  """Check that nested tags (with ':') have their parent tags present.

  Args:
    quiet: If True, suppress detailed output

  Returns:
    Number of modules with missing parent tags.
  """
  missing_parent_tags_by_module = {}

  for module_name, tags in MODULE_TAGS.items():
    tags_set = set(tags)
    missing_parents = []
    for tag in tags:
      if ':' in tag:
        parent = tag.split(':')[0]
        if parent not in tags_set:
          missing_parents.append(f"{tag} (missing parent: {parent})")
    if missing_parents:
      missing_parent_tags_by_module[module_name] = missing_parents

  if not quiet:
    if missing_parent_tags_by_module:
      total_missing = sum(
          len(tags) for tags in missing_parent_tags_by_module.values())
      print(
          f"\nFound {total_missing} nested tag(s) missing parent tags in {len(missing_parent_tags_by_module)} module(s):\n"
      )
      for module_name in sorted(missing_parent_tags_by_module.keys()):
        print(f"  {module_name}:")
        for tag_msg in sorted(missing_parent_tags_by_module[module_name]):
          print(f"    - {tag_msg}")
      print(
          f"\nNested tags (e.g., 'power:battery') must include their parent tag (e.g., 'power')"
      )
    else:
      print(f"\nAll nested tags have their parent tags!")

  return len(missing_parent_tags_by_module)


def main() -> int:
  parser = argparse.ArgumentParser(
      description="Check stdlib modules for banned patterns and documentation")
  parser.add_argument(
      '--stdlib-sources',
      default=os.path.join(ROOT_DIR, "src", "trace_processor", "perfetto_sql",
                           "stdlib"))
  parser.add_argument(
      '--verbose',
      action='store_true',
      default=False,
      help='Enable additional logging')
  parser.add_argument(
      '--name-filter',
      default=None,
      type=str,
      help='Filter the name of the modules to check (regex syntax)')
  parser.add_argument(
      '--check-includes',
      action='store_true',
      default=False,
      help='Also check that modules properly declare their dependencies via INCLUDE statements'
  )
  parser.add_argument(
      '--check-tags',
      action='store_true',
      default=False,
      help='Check that all modules with public artifacts have tags defined')
  parser.add_argument(
      '--check-orphaned-tags',
      action='store_true',
      default=False,
      help='Check that all tags in MODULE_TAGS correspond to actual modules')

  args = parser.parse_args()

  # Parse all modules once with internal artifacts included
  # We need internal artifacts for dependency checking, but will filter them
  # out when checking schemas (internal artifacts don't need schemas)
  modules = parse_all_modules(
      stdlib_path=args.stdlib_sources,
      include_internal=True,
      name_filter=args.name_filter)

  if args.verbose:
    for abs_path, rel_path, _, parsed in modules:
      obj_count = (
          len(parsed.functions) + len(parsed.table_functions) +
          len(parsed.table_views) + len(parsed.macros))
      print(f"Parsing '{rel_path}' ({obj_count} objects, "
            f"{len(parsed.errors)} errors) - "
            f"{len(parsed.functions)} functions, "
            f"{len(parsed.table_functions)} table functions, "
            f"{len(parsed.table_views)} tables/views, "
            f"{len(parsed.macros)} macros.")

  all_errors = 0
  for abs_path, rel_path, _, parsed in modules:
    errors = []

    # Read SQL content
    with open(abs_path, 'r', encoding='utf-8') as f:
      sql = f.read()

    # Check for banned statements
    lines = [l.strip() for l in sql.split('\n')]
    for line in lines:
      if line.startswith('--'):
        continue
      if 'run_metric' in line.casefold():
        errors.append("RUN_METRIC is banned in standard library.")
      if 'insert into' in line.casefold():
        errors.append("INSERT INTO table is not allowed in standard library.")

    # Validate includes
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
            f"No modules can depend on '{PKG_VIZ}' outside '{PKG_VIZ}' package."
        )

      if package == PKG_CHROME and include_package == PKG_ANDROID:
        errors.append(
            f"Modules from package '{PKG_CHROME}' can't include '{include.module}' "
            f"from package '{PKG_ANDROID}'")

      if package == PKG_ANDROID and include_package == PKG_CHROME:
        errors.append(
            f"Modules from package '{PKG_ANDROID}' can't include '{include.module}' "
            f"from package '{PKG_CHROME}'")

    # Add parsing errors and validation errors
    errors += [
        *parsed.errors, *check_banned_words(sql),
        *check_banned_create_table_as(sql), *check_banned_create_view_as(sql),
        *check_banned_include_all(sql), *check_banned_drop(sql)
    ]

    if errors:
      sys.stderr.write(f"\nFound {len(errors)} errors in file "
                       f"'{os.path.normpath(abs_path)}':\n- ")
      sys.stderr.write("\n- ".join(errors))
      sys.stderr.write("\n\n")

    all_errors += len(errors)

  # Check includes if requested
  include_errors = 0
  if args.check_includes:
    include_errors = check_includes(modules, quiet=not args.verbose)

  # Check tags if requested
  tag_errors = 0
  invalid_tag_errors = 0
  nested_tag_errors = 0
  if args.check_tags:
    # Always check for invalid tags and nested tag parents when checking tags
    invalid_tag_errors = check_invalid_tags(quiet=not args.verbose)
    nested_tag_errors = check_nested_tag_parents(quiet=not args.verbose)
    tag_errors = check_tags(modules, quiet=not args.verbose)

  # Check orphaned tags if requested
  orphaned_tag_errors = 0
  if args.check_orphaned_tags:
    orphaned_tag_errors = check_orphaned_tags(modules, quiet=not args.verbose)

  total_errors = all_errors + include_errors + tag_errors + orphaned_tag_errors + invalid_tag_errors + nested_tag_errors
  return 0 if not total_errors else 1


if __name__ == "__main__":
  sys.exit(main())
