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
from typing import List

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.pfsql import parse_modules
from python.generators.sql_processing.stdlib_parser import parse_all_modules
from python.generators.sql_processing.utils import check_banned_usage
from python.generators.sql_processing.utils import is_internal

# Package name constants
PKG_PRELUDE = "prelude"
PKG_DEPRECATED = "deprecated"

# Frozen set of allowed top-level public packages.
#
# This list is intentionally frozen to avoid namespace clashes with server
# extensions which will increasingly add their own top-level packages.
#
# Historically, core trace processor functionality (e.g. intervals, graphs,
# counters, slices) should have been placed under a 'std' top-level package,
# but migration would be too disruptive. Instead, we freeze the current set
# and require future core packages to be placed under 'std'.
#
# Platform-specific packages (e.g. 'android') may still be added as exceptions
# since they are "closer to the platform" than core to Perfetto. Contact
# lalitm@ for such exceptions.
ALLOWED_TOPLEVEL_PUBLIC_PACKAGES = frozenset({
    "android",
    "appleos",
    "chrome",
    "counters",
    "export",
    "graphs",
    "intervals",
    "linux",
    "pixel",
    "pkvm",
    "prelude",
    "sched",
    "slices",
    "stacks",
    "std",
    "time",
    "traced",
    "v8",
    "wattson",
})

# Packages that are OS, platform, product, or engine specific. Generic packages
# (see GENERIC_PACKAGES) must never depend on these: doing so is a layering
# violation because it drags platform-specific concepts into building blocks
# that are meant to work on any trace.
PLATFORM_SPECIFIC_PACKAGES = frozenset({
    "android",
    "appleos",
    "chrome",
    "linux",
    "pixel",
    "pkvm",
    "traced",
    "v8",
    "wattson",
})

# Generic, platform-agnostic packages. These are the core building blocks that
# operate over the base trace_processor tables and must not reach "up" into any
# platform-specific package.
GENERIC_PACKAGES = frozenset({
    "callstacks",
    "counters",
    "graphs",
    "intervals",
    "sched",
    "slices",
    "stacks",
    "std",
    "time",
})

# Sanctioned exceptions to the layering rule: specific generic ->
# platform-specific dependencies that we intentionally accept for now.
#
# These are not the end state. Each is a place where a generic building block is
# coupled to a platform-specific module and we would like to do better in the
# future (e.g. by moving the shared logic somewhere neutral). Listing them
# explicitly keeps the check green while still catching any NEW violation, and
# makes the debt we want to pay down visible.
#
# Each entry is (module_name, included_module_name).
ALLOWED_LAYERING_EXCEPTIONS = frozenset({
    # callstacks.stack_profile annotates frames with V8 JIT information.
    ("callstacks.stack_profile", "v8.jit"),
    # slices.cpu_time.thread_slice_cpu_cycles is a thin wrapper over
    # linux.cpu.utilization.slice.cpu_cycles_per_thread_slice.
    ("slices.cpu_time", "linux.cpu.utilization.slice"),
})


def check_includes(all_modules: List[dict],
                   modules: List[dict],
                   quiet: bool = False) -> int:
  """Check that modules properly declare their dependencies.

  Args:
    all_modules: Every module of the stdlib from `pfsql parse`
    modules: The modules to check
    quiet: If True, suppress detailed output

  Returns:
    Number of modules with include errors.
  """
  defining_module = {}
  for module in all_modules:
    for symbol in module['symbols']:
      defining_module.setdefault(symbol['name'], module['module'])

  modules_with_errors = 0
  for module in modules:
    # Prelude modules are exempt: they provide core functionality that is
    # automatically available to all modules.
    is_prelude = module['module'].startswith(f'{PKG_PRELUDE}.')

    own_symbols = {s['name'] for s in module['symbols']}
    references = set(module['references'])
    for symbol in module['symbols']:
      references.update(symbol['references'])

    used_modules = defaultdict(set)
    for name in references - own_symbols:
      target = defining_module.get(name)
      if target and not target.startswith(f'{PKG_PRELUDE}.'):
        used_modules[target].add(name)

    includes = module['includes']
    unused_imports = [] if is_prelude else sorted(
        set(includes) - set(used_modules))
    silent_imports = [] if is_prelude else sorted(
        f'{name} (from {target})' for target, names in used_modules.items()
        if target not in includes for name in names)
    errors = module['errors']
    if not (unused_imports or silent_imports or errors):
      continue
    modules_with_errors += 1

    print(f"Module: {module['module']}")
    for title, items in (('Unused imports', unused_imports),
                         ('Silent imports', silent_imports), ('Errors',
                                                              errors)):
      if items:
        print(f"  {title} ({len(items)}):")
        for item in items:
          print(f"    - {item}")
    print()

  if not quiet and not modules_with_errors:
    print("\nNo include errors found in any modules!")
  return modules_with_errors


def check_new_packages(modules: List[dict], quiet: bool = False) -> int:
  """Check that no new top-level public packages have been added.

  This check ensures that all top-level packages with public artifacts are
  in the allowed list. New packages should be placed under the 'std' folder.

  Args:
    modules: Modules from `pfsql parse`
    quiet: If True, suppress detailed output

  Returns:
    Number of disallowed new packages found.
  """
  packages_with_public = {
      module['module'].split('.')[0]
      for module in modules
      if any(not is_internal(s['name']) for s in module['symbols'])
  }
  packages_with_public.discard(PKG_DEPRECATED)

  # Find packages not in the allowed list
  disallowed_packages = packages_with_public - ALLOWED_TOPLEVEL_PUBLIC_PACKAGES

  if not quiet:
    if disallowed_packages:
      print(f"\nFound {len(disallowed_packages)} disallowed new top-level "
            f"public package(s):\n")
      for pkg in sorted(disallowed_packages):
        print(f"  - {pkg}")
      print(
          f"\nTop-level public packages are frozen to avoid namespace clashes "
          f"with server extensions.")
      print(
          f"\nCore trace processor packages (like intervals, graphs, counters) "
          f"should be placed under 'std' (e.g. std.{list(disallowed_packages)[0]})."
      )
      print(f"\nPlatform-specific packages (like 'android') may be excepted. "
            f"Contact lalitm@ for exceptions.")
    else:
      print(f"\nNo disallowed new top-level public packages found!")

  return len(disallowed_packages)


def check_layering(modules: List[dict], quiet: bool = False) -> int:
  """Check that generic packages do not depend on platform-specific ones.

  A generic package (e.g. intervals, graphs, slices) is a building block meant
  to work on any trace. Depending on a platform-specific package (e.g. android,
  linux, chrome) is a layering violation: it drags platform concepts into code
  that should be platform-agnostic and creates surprising, unwanted dependency
  chains.

  Args:
    modules: Modules from `pfsql parse`
    quiet: If True, suppress detailed output

  Returns:
    Number of layering violations found.
  """
  violations = []
  for module in modules:
    module_name = module['module']
    if module_name.split('.')[0] not in GENERIC_PACKAGES:
      continue
    for included in module['includes']:
      if included.split('.')[0] not in PLATFORM_SPECIFIC_PACKAGES:
        continue
      if (module_name, included) in ALLOWED_LAYERING_EXCEPTIONS:
        continue
      violations.append((module_name, included))

  if not quiet:
    if violations:
      print(f"\nFound {len(violations)} layering violation(s):\n")
      for module_name, included in sorted(violations):
        src_pkg = module_name.split('.')[0]
        dst_pkg = included.split('.')[0]
        print(f"  {module_name} (generic '{src_pkg}') depends on "
              f"{included} (platform-specific '{dst_pkg}')")
      print(
          f"\nGeneric packages must not depend on platform-specific packages. "
          f"Remove the INCLUDE if it is unused, or move the code into the "
          f"platform-specific package. If the dependency is genuinely "
          f"intentional, add it to ALLOWED_LAYERING_EXCEPTIONS in "
          f"{os.path.basename(__file__)}.")
    else:
      print(f"\nNo layering violations found!")

  return len(violations)


def main() -> int:
  parser = argparse.ArgumentParser(
      description="Check stdlib modules for banned patterns and documentation")
  parser.add_argument(
      '--pfsql-binary',
      required=True,
      help='Path to a pfsql binary built from this checkout, to read the SQL with'
  )
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
      '--check-new-packages',
      action='store_true',
      default=False,
      help='Check that no new top-level public packages have been added')
  parser.add_argument(
      '--check-layering',
      action='store_true',
      default=False,
      help='Check that generic packages do not depend on platform-specific ones'
  )

  args = parser.parse_args()

  # Internal artifacts are needed for dependency checking.
  all_parsed = parse_modules(args.stdlib_sources, args.pfsql_binary)
  modules = parse_all_modules(
      stdlib_path=args.stdlib_sources,
      pfsql_binary=args.pfsql_binary,
      include_internal=True,
      name_filter=args.name_filter,
      pfsql_modules=all_parsed)
  parsed_by_name = {m['module']: m for m in all_parsed}

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
  for abs_path, _, module_name, parsed in modules:
    errors = check_banned_usage(parsed, parsed_by_name[module_name])

    if errors:
      sys.stderr.write(f"\nFound {len(errors)} errors in file "
                       f"'{os.path.normpath(abs_path)}':\n- ")
      sys.stderr.write("\n- ".join(errors))
      sys.stderr.write("\n\n")

    all_errors += len(errors)

  include_errors = 0
  new_package_errors = 0
  layering_errors = 0
  pattern = re.compile(args.name_filter) if args.name_filter else None
  parsed = [m for m in all_parsed if not pattern or pattern.match(m['path'])]
  quiet = not args.verbose
  if args.check_includes:
    include_errors = check_includes(all_parsed, parsed, quiet)
  if args.check_new_packages:
    new_package_errors = check_new_packages(parsed, quiet)
  if args.check_layering:
    layering_errors = check_layering(parsed, quiet)

  total_errors = all_errors + include_errors + new_package_errors + layering_errors
  return 0 if not total_errors else 1


if __name__ == "__main__":
  sys.exit(main())
