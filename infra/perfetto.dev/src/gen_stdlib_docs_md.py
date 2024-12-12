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
# disibuted under the License is disibuted on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import sys
import json
from typing import Any, List, Dict

INTRODUCTION = '''
# PerfettoSQL standard library
*This page documents the PerfettoSQL standard library.*

## Introduction
The PerfettoSQL standard library is a repository of tables, views, functions
and macros, contributed by domain experts, which make querying traces easier
Its design is heavily inspired by standard libraries in languages like Python,
C++ and Java.

Some of the purposes of the standard library include:
1) Acting as a way of sharing and commonly written queries without needing
to copy/paste large amounts of SQL.
2) Raising the abstraction level when exposing data in the trace. Many
modules in the standard library convert low-level trace concepts
e.g. slices, tracks and into concepts developers may be more familar with
e.g. for Android developers: app startups, binder transactions etc.

Standard library modules can be included as follows:
```
-- Include all tables/views/functions from the android.startup.startups
-- module in the standard library.
INCLUDE PERFETTO MODULE android.startup.startups;

-- Use the android_startups table defined in the android.startup.startups
-- module.
SELECT *
FROM android_startups;
```

Prelude is a special module is automatically included. It contains key helper
tables, views and functions which are universally useful.

More information on importing modules is available in the
[syntax documentation](/docs/analysis/perfetto-sql-syntax#including-perfettosql-modules)
for the `INCLUDE PERFETTO MODULE` statement.

<!-- TODO(b/290185551): talk about experimental module and contributions. -->
'''


def _escape(desc: str) -> str:
  """Escapes special characters in a markdown table."""
  return desc.replace('|', '\\|')


def _md_table_header(cols: List[str]) -> str:
  col_str = ' | '.join(cols) + '\n'
  lines = ['-' * len(col) for col in cols]
  underlines = ' | '.join(lines)
  return col_str + underlines


def _md_rolldown(summary: str, content: str) -> str:
  return f"""<details>
  <summary style="cursor: pointer;">{summary}</summary>

  {content}

  </details>
  """


def _bold(s: str) -> str:
  return f"<strong>{s}</strong>"


class ModuleMd:
  """Responsible for module level markdown generation."""

  def __init__(self, package_name: str, module_dict: Dict):
    self.module_name = module_dict['module_name']
    self.include_str = self.module_name if package_name != 'prelude' else 'N/A'
    self.objs, self.funs, self.view_funs, self.macros = [], [], [], []

    # Views/tables
    for data in module_dict['data_objects']:
      if not data['cols']:
        continue

      obj_summary = (
          f'''{_bold(data['name'])}. {data['summary_desc']}\n'''
      )
      content = [f"{data['type']}"]
      if (data['summary_desc'] != data['desc']):
        content.append(data['desc'])

      table = [_md_table_header(['Column', 'Type', 'Description'])]
      for info in data['cols']:
        name = info["name"]
        table.append(
            f'{name} | {info["type"]} | {_escape(info["desc"])}')
      content.append('\n\n')
      content.append('\n'.join(table))

      self.objs.append(_md_rolldown(obj_summary, '\n'.join(content)))

      self.objs.append('\n\n')

    # Functions
    for d in module_dict['functions']:
      summary = f'''{_bold(d['name'])} -> {d['return_type']}. {d['summary_desc']}\n\n'''
      content = []
      if (d['summary_desc'] != d['desc']):
        content.append(d['desc'])

      content.append(
          f"Returns {d['return_type']}: {d['return_desc']}\n\n")
      if d['args']:
        content.append(_md_table_header(['Argument', 'Type', 'Description']))
        for arg_dict in d['args']:
          content.append(
              f'''{arg_dict['name']} | {arg_dict['type']} | {_escape(arg_dict['desc'])}'''
          )

      self.funs.append(_md_rolldown(summary, '\n'.join(content)))
      self.funs.append('\n\n')

    # Table functions
    for data in module_dict['table_functions']:
      obj_summary = f'''{_bold(data['name'])}. {data['summary_desc']}\n\n'''
      content = []
      if (data['summary_desc'] != data['desc']):
        content.append(data['desc'])

      if data['args']:
        args_table = [_md_table_header(['Argument', 'Type', 'Description'])]
        for arg_dict in data['args']:
          args_table.append(
              f'''{arg_dict['name']} | {arg_dict['type']} | {_escape(arg_dict['desc'])}'''
          )
        content.append('\n'.join(args_table))
        content.append('\n\n')

      content.append(_md_table_header(['Column', 'Type', 'Description']))
      for column in data['cols']:
        content.append(
            f'{column["name"]} | {column["type"]} | {column["desc"]}')

      self.view_funs.append(_md_rolldown(obj_summary, '\n'.join(content)))
      self.view_funs.append('\n\n')

    # Macros
    for data in module_dict['macros']:
      obj_summary = f'''{_bold(data['name'])}. {data['summary_desc']}\n\n'''
      content = []
      if (data['summary_desc'] != data['desc']):
        content.append(data['desc'])

      content.append(
          f'''Returns: {data['return_type']}, {data['return_desc']}\n\n''')
      if data['args']:
        table = [_md_table_header(['Argument', 'Type', 'Description'])]
        for arg_dict in data['args']:
          table.append(
              f'''{arg_dict['name']} | {arg_dict['type']} | {_escape(arg_dict['desc'])}'''
          )
        content.append('\n'.join(table))

      self.macros.append(_md_rolldown(obj_summary, '\n'.join(content)))
      self.macros.append('\n\n')


class PackageMd:
  """Responsible for package level markdown generation."""

  def __init__(self, package_name: str, module_files: List[Dict[str,
                                                                Any]]) -> None:
    self.package_name = package_name
    self.modules_md = sorted(
        [ModuleMd(package_name, file_dict) for file_dict in module_files],
        key=lambda x: x.module_name)

  def get_prelude_description(self) -> str:
    if not self.package_name == 'prelude':
      raise ValueError("Only callable on prelude module")

    lines = []
    lines.append(f'## Package: {self.package_name}')

    # Prelude is a special module which is automatically imported and doesn't
    # have any include keys.
    objs = '\n'.join(obj for module in self.modules_md for obj in module.objs)
    if objs:
      lines.append('#### Views/Tables')
      lines.append(objs)

    funs = '\n'.join(fun for module in self.modules_md for fun in module.funs)
    if funs:
      lines.append('#### Functions')
      lines.append(funs)

    table_funs = '\n'.join(
        view_fun for module in self.modules_md for view_fun in module.view_funs)
    if table_funs:
      lines.append('#### Table Functions')
      lines.append(table_funs)

    macros = '\n'.join(
        macro for module in self.modules_md for macro in module.macros)
    if macros:
      lines.append('#### Macros')
      lines.append(macros)

    return '\n'.join(lines)

  def get_md(self) -> str:
    if not self.modules_md:
      return ''

    if self.package_name == 'prelude':
      raise ValueError("Can't be called with prelude module")

    lines = []
    lines.append(f'## Package: {self.package_name}')

    for file in self.modules_md:
      if not any((file.objs, file.funs, file.view_funs, file.macros)):
        continue

      lines.append(f'### {file.module_name}')
      if file.objs:
        lines.append('#### Views/Tables')
        lines.append('\n'.join(file.objs))
      if file.funs:
        lines.append('#### Functions')
        lines.append('\n'.join(file.funs))
      if file.view_funs:
        lines.append('#### Table Functions')
        lines.append('\n'.join(file.view_funs))
      if file.macros:
        lines.append('#### Macros')
        lines.append('\n'.join(file.macros))

    return '\n'.join(lines)

  def is_empty(self) -> bool:
    for file in self.modules_md:
      if any((file.objs, file.funs, file.view_funs, file.macros)):
        return False
    return True


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--input', required=True)
  parser.add_argument('--output', required=True)
  args = parser.parse_args()

  with open(args.input) as f:
    stdlib_json = json.load(f)

  # Fetch the modules from json documentation.
  packages: Dict[str, PackageMd] = {}
  for package in stdlib_json:
    package_name = package["name"]
    modules = package["modules"]
    # Remove 'common' when it has been removed from the code.
    if package_name not in ['deprecated', 'common']:
      package = PackageMd(package_name, modules)
      if (not package.is_empty()):
        packages[package_name] = package

  prelude = packages.pop('prelude')

  with open(args.output, 'w') as f:
    f.write(INTRODUCTION)
    f.write(prelude.get_prelude_description())
    f.write('\n')
    f.write('\n'.join(module.get_md() for module in packages.values()))

  return 0


if __name__ == '__main__':
  sys.exit(main())
