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

Prelude is a special module is automatically imported. It contains key helper
tables, views and functions which are universally useful.

More information on importing modules is available in the
[syntax documentation](/docs/analysis/perfetto-sql-syntax#including-perfettosql-modules)
for the `INCLUDE PERFETTO MODULE` statement.

<!-- TODO(b/290185551): talk about experimental module and contributions. -->

## Summary
'''


def _escape_in_table(desc: str):
  """Escapes special characters in a markdown table."""
  return desc.replace('|', '\\|')


def _md_table(cols: List[str]):
  col_str = ' | '.join(cols) + '\n'
  lines = ['-' * len(col) for col in cols]
  underlines = ' | '.join(lines)
  return col_str + underlines


def _write_summary(sql_type: str, table_cols: List[str],
                   summary_objs: List[str]) -> str:
  table_data = '\n'.join(s.strip() for s in summary_objs if s)
  return f"""
### {sql_type}

{_md_table(table_cols)}
{table_data}

"""


class FileMd:
  """Responsible for file level markdown generation."""

  def __init__(self, module_name, file_dict):
    self.import_key = file_dict['import_key']
    import_key_name = self.import_key if module_name != 'prelude' else 'N/A'
    self.objs, self.funs, self.view_funs, self.macros = [], [], [], []
    summary_objs_list, summary_funs_list, summary_view_funs_list, summary_macros_list = [], [], [], []

    # Add imports if in file.
    for data in file_dict['imports']:
      # Anchor
      anchor = f'''obj/{module_name}/{data['name']}'''

      # Add summary of imported view/table
      summary_objs_list.append(f'''[{data['name']}](#{anchor})|'''
                               f'''{import_key_name}|'''
                               f'''{_escape_in_table(data['summary_desc'])}''')

      self.objs.append(f'''\n\n<a name="{anchor}"></a>'''
                       f'''**{data['name']}**, {data['type']}\n\n'''
                       f'''{_escape_in_table(data['desc'])}\n''')

      self.objs.append(_md_table(['Column', 'Type', 'Description']))
      for name, info in data['cols'].items():
        self.objs.append(
            f'{name} | {info["type"]} | {_escape_in_table(info["desc"])}')

      self.objs.append('\n\n')

    # Add functions if in file
    for data in file_dict['functions']:
      # Anchor
      anchor = f'''fun/{module_name}/{data['name']}'''

      # Add summary of imported function
      summary_funs_list.append(f'''[{data['name']}](#{anchor})|'''
                               f'''{import_key_name}|'''
                               f'''{data['return_type']}|'''
                               f'''{_escape_in_table(data['summary_desc'])}''')
      self.funs.append(
          f'''\n\n<a name="{anchor}"></a>'''
          f'''**{data['name']}**\n\n'''
          f'''{data['desc']}\n\n'''
          f'''Returns: {data['return_type']}, {data['return_desc']}\n\n''')
      if data['args']:
        self.funs.append(_md_table(['Argument', 'Type', 'Description']))
        for name, arg_dict in data['args'].items():
          self.funs.append(
              f'''{name} | {arg_dict['type']} | {_escape_in_table(arg_dict['desc'])}'''
          )

        self.funs.append('\n\n')

    # Add table functions if in file
    for data in file_dict['table_functions']:
      # Anchor
      anchor = rf'''view_fun/{module_name}/{data['name']}'''
      # Add summary of imported view function
      summary_view_funs_list.append(
          f'''[{data['name']}](#{anchor})|'''
          f'''{import_key_name}|'''
          f'''{_escape_in_table(data['summary_desc'])}''')

      self.view_funs.append(f'''\n\n<a name="{anchor}"></a>'''
                            f'''**{data['name']}**\n'''
                            f'''{data['desc']}\n\n''')
      if data['args']:
        self.funs.append(_md_table(['Argument', 'Type', 'Description']))
        for name, arg_dict in data['args'].items():
          self.view_funs.append(
              f'''{name} | {arg_dict['type']} | {_escape_in_table(arg_dict['desc'])}'''
          )
        self.view_funs.append('\n')
        self.view_funs.append(_md_table(['Column', 'Type', 'Description']))
      for name, column in data['cols'].items():
        self.view_funs.append(f'{name} | {column["type"]} | {column["desc"]}')

      self.view_funs.append('\n\n')

    # Add macros if in file
    for data in file_dict['macros']:
      # Anchor
      anchor = rf'''macro/{module_name}/{data['name']}'''
      # Add summary of imported view function
      summary_macros_list.append(
          f'''[{data['name']}](#{anchor})|'''
          f'''{import_key_name}|'''
          f'''{_escape_in_table(data['summary_desc'])}''')

      self.macros.append(
          f'''\n\n<a name="{anchor}"></a>'''
          f'''**{data['name']}**\n'''
          f'''{data['desc']}\n\n'''
          f'''Returns: {data['return_type']}, {data['return_desc']}\n\n''')
      if data['args']:
        self.macros.append(_md_table(['Argument', 'Type', 'Description']))
        for name, arg_dict in data['args'].items():
          self.macros.append(
              f'''{name} | {arg_dict['type']} | {_escape_in_table(arg_dict['desc'])}'''
          )
        self.macros.append('\n')
      self.macros.append('\n\n')

    self.summary_objs = '\n'.join(summary_objs_list)
    self.summary_funs = '\n'.join(summary_funs_list)
    self.summary_view_funs = '\n'.join(summary_view_funs_list)
    self.summary_macros = '\n'.join(summary_macros_list)


class ModuleMd:
  """Responsible for module level markdown generation."""

  def __init__(self, module_name: str, module_files: List[Dict[str,
                                                               Any]]) -> None:
    self.module_name = module_name
    self.files_md = sorted(
        [FileMd(module_name, file_dict) for file_dict in module_files],
        key=lambda x: x.import_key)
    self.summary_objs = '\n'.join(
        file.summary_objs for file in self.files_md if file.summary_objs)
    self.summary_funs = '\n'.join(
        file.summary_funs for file in self.files_md if file.summary_funs)
    self.summary_view_funs = '\n'.join(file.summary_view_funs
                                       for file in self.files_md
                                       if file.summary_view_funs)
    self.summary_macros = '\n'.join(
        file.summary_macros for file in self.files_md if file.summary_macros)

  def get_prelude_description(self) -> str:
    if not self.module_name == 'prelude':
      raise ValueError("Only callable on prelude module")

    lines = []
    lines.append(f'## Module: {self.module_name}')

    # Prelude is a special module which is automatically imported and doesn't
    # have any include keys.
    objs = '\n'.join(obj for file in self.files_md for obj in file.objs)
    if objs:
      lines.append('#### Views/Tables')
      lines.append(objs)

    funs = '\n'.join(fun for file in self.files_md for fun in file.funs)
    if funs:
      lines.append('#### Functions')
      lines.append(funs)

    table_funs = '\n'.join(
        view_fun for file in self.files_md for view_fun in file.view_funs)
    if table_funs:
      lines.append('#### Table Functions')
      lines.append(table_funs)

    macros = '\n'.join(macro for file in self.files_md for macro in file.macros)
    if macros:
      lines.append('#### Macros')
      lines.append(macros)

    return '\n'.join(lines)

  def get_description(self) -> str:
    if not self.files_md:
      return ''

    if self.module_name == 'prelude':
      raise ValueError("Can't be called with prelude module")

    lines = []
    lines.append(f'## Module: {self.module_name}')

    for file in self.files_md:
      if not any((file.objs, file.funs, file.view_funs, file.macros)):
        continue

      lines.append(f'### {file.import_key}')
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


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--input', required=True)
  parser.add_argument('--output', required=True)
  args = parser.parse_args()

  with open(args.input) as f:
    modules_json_dict = json.load(f)

  # Fetch the modules from json documentation.
  modules_dict: Dict[str, ModuleMd] = {}
  for module_name, module_files in modules_json_dict.items():
    # Remove 'common' when it has been removed from the code.
    if module_name not in ['deprecated', 'common']:
      modules_dict[module_name] = ModuleMd(module_name, module_files)

  prelude_module = modules_dict.pop('prelude')

  with open(args.output, 'w') as f:
    f.write(INTRODUCTION)

    summary_objs = [prelude_module.summary_objs
                   ] if prelude_module.summary_objs else []
    summary_objs += [
        module.summary_objs
        for module in modules_dict.values()
        if (module.summary_objs)
    ]

    summary_funs = [prelude_module.summary_funs
                   ] if prelude_module.summary_funs else []
    summary_funs += [module.summary_funs for module in modules_dict.values()]
    summary_view_funs = [prelude_module.summary_view_funs
                        ] if prelude_module.summary_view_funs else []
    summary_view_funs += [
        module.summary_view_funs for module in modules_dict.values()
    ]
    summary_macros = [prelude_module.summary_macros
                     ] if prelude_module.summary_macros else []
    summary_macros += [
        module.summary_macros for module in modules_dict.values()
    ]

    if summary_objs:
      f.write(
          _write_summary('Views/tables', ['Name', 'Import', 'Description'],
                         summary_objs))

    if summary_funs:
      f.write(
          _write_summary('Functions',
                         ['Name', 'Import', 'Return type', 'Description'],
                         summary_funs))

    if summary_view_funs:
      f.write(
          _write_summary('Table functions', ['Name', 'Import', 'Description'],
                         summary_view_funs))

    if summary_macros:
      f.write(
          _write_summary('Macros', ['Name', 'Import', 'Description'],
                         summary_macros))

    f.write('\n\n')
    f.write(prelude_module.get_prelude_description())
    f.write('\n')
    f.write('\n'.join(
        module.get_description() for module in modules_dict.values()))

  return 0


if __name__ == '__main__':
  sys.exit(main())
