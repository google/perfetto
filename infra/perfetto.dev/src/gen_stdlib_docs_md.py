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
from typing import List, Dict


# Responsible for module level markdown generation.
class ModuleMd:

  def __init__(self, module_name: str,
               module_files: List[Dict[str, any]]) -> None:
    self.module_name = module_name
    self.files_md = [
        FileMd(module_name, file_dict) for file_dict in module_files
    ]
    self.summary_objs = "\n".join(
        file.summary_objs for file in self.files_md if file.summary_objs)
    self.summary_funs = "\n".join(
        file.summary_funs for file in self.files_md if file.summary_funs)
    self.summary_view_funs = "\n".join(file.summary_view_funs
                                       for file in self.files_md
                                       if file.summary_view_funs)

  def print_description(self):
    long_s = []
    long_s.append(f'## Module: {self.module_name}')

    for file in self.files_md:
      long_s.append(f'### {file.import_key}')
      if file.objs:
        long_s.append('#### Views/Tables')
        long_s.append("\n".join(file.objs))
      if file.funs:
        long_s.append('#### Functions')
        long_s.append("\n".join(file.funs))
      if file.view_funs:
        long_s.append('#### View Functions')
        long_s.append("\n".join(file.view_funs))

    return '\n'.join(long_s)


# Responsible for file level markdown generation.
class FileMd:

  def __init__(self, module_name, file_dict):
    self.import_key = file_dict['import_key']
    self.objs, self.funs, self.view_funs = [], [], []
    summary_objs_list, summary_funs_list, summary_view_funs_list = [], [], []

    # Add imports if in file.
    for data in file_dict['imports']:
      # Anchor
      anchor = f'obj/{module_name}/{data["name"]}'

      # Add summary of imported view/table
      desc = data["desc"].split(".")[0]
      summary_objs_list.append(f'[{data["name"]}](#{anchor})|'
                               f'{file_dict["import_key"]}|'
                               f'{desc}')

      self.objs.append(f'\n\n<a name="{anchor}"></a>'
                       f'**{data["name"]}**, {data["type"]}\n\n'
                       f'{data["desc"]}\n')

      self.objs.append('Column | Description\n------ | -----------')
      for name, desc in data['cols'].items():
        self.objs.append(f'{name} | {desc}')

      self.objs.append("\n\n")

    # Add functions if in file
    for data in file_dict['functions']:
      # Anchor
      anchor = f'fun/{module_name}/{data["name"]}'

      # Add summary of imported function
      summary_funs_list.append(f'[{data["name"]}](#{anchor})|'
                               f'{file_dict["import_key"]}|'
                               f'{data["return_type"]}|'
                               f'{data["desc"].split(".")[0]}')
      self.funs.append(
          f'\n\n<a name="{anchor}"></a>'
          f'**{data["name"]}**\n'
          f'{data["desc"]}\n\n'
          f'Returns: {data["return_type"]}, {data["return_desc"]}\n\n')
      if data['args']:
        self.funs.append('Argument | Type | Description\n'
                         '-------- | ---- | -----------')
        for name, arg_dict in data['args'].items():
          self.funs.append(f'{name} | {arg_dict["type"]} | {arg_dict["desc"]}')

        self.funs.append("\n\n")

    # Add view functions if in file
    for data in file_dict['view_functions']:
      # Anchor
      anchor = rf'view_fun/{module_name}/{data["name"]}'
      # Add summary of imported view function
      summary_view_funs_list.append(f'[{data["name"]}](#{anchor})|'
                                    f'{file_dict["import_key"]}|'
                                    f'{data["desc"].split(".")[0]}')

      self.view_funs.append(f'\n\n<a name="{anchor}"></a>'
                            f'**{data["name"]}**\n'
                            f'{data["desc"]}\n\n')
      if data['args']:
        self.view_funs.append('Argument | Type | Description\n'
                              '-------- | ---- | -----------')
        for name, arg_dict in data['args'].items():
          self.view_funs.append(
              f'{name} | {arg_dict["type"]} | {arg_dict["desc"]}')
        self.view_funs.append('\n')
      self.view_funs.append('Column | Description\n' '------ | -----------')
      for name, desc in data['cols'].items():
        self.view_funs.append(f'{name} | {desc}')

      self.view_funs.append("\n\n")

    self.summary_objs = "\n".join(summary_objs_list)
    self.summary_funs = "\n".join(summary_funs_list)
    self.summary_view_funs = "\n".join(summary_view_funs_list)


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--input', required=True)
  parser.add_argument('--output', required=True)
  args = parser.parse_args()

  with open(args.input) as f:
    modules_json_dict = json.load(f)

  modules_dict = {}

  for module_name, module_files in modules_json_dict.items():
    modules_dict[module_name] = ModuleMd(module_name, module_files)

  common_module = modules_dict.pop('common')

  with open(args.output, 'w') as f:
    f.write("# SQL standard library\n"
            "To import any function, view_function, view or table simply run "
            "`SELECT IMPORT({import key});` in your SQL query.\n"
            "## Summary\n")

    summary_objs = [common_module.summary_objs
                   ] if common_module.summary_objs else []
    summary_objs += [
        module.summary_objs
        for name, module in modules_dict.items()
        if (module.summary_objs and name != 'experimental')
    ]

    summary_funs = [common_module.summary_funs
                   ] if common_module.summary_funs else []
    summary_funs += [
        module.summary_funs
        for name, module in modules_dict.items()
        if (module.summary_funs and name != 'experimental')
    ]
    summary_view_funs = [common_module.summary_view_funs
                        ] if common_module.summary_view_funs else []
    summary_view_funs += [
        module.summary_view_funs
        for name, module in modules_dict.items()
        if (module.summary_view_funs and name != 'experimental')
    ]

    if summary_objs:
      f.write('### Views/tables\n\n'
              'Name | Import | Description\n'
              '---- | ------ | -----------\n')
      f.write('\n'.join(summary_objs))
      f.write("\n")

    if summary_funs:
      f.write("### Functions\n\n"
              'Name | Import | Return type | Description\n'
              '---- | ------ | ----------- | -----------\n')
      f.write('\n'.join(summary_funs))
      f.write("\n")

    if summary_view_funs:
      f.write("### View Functions\n\n"
              'Name | Import |  Description\n'
              '---- | ------ |  -----------\n')
      f.write('\n'.join(summary_view_funs))
      f.write("\n")

    f.write('\n\n')
    f.write(common_module.print_description())
    f.write('\n')
    f.write('\n'.join(
        module.print_description() for module in modules_dict.values()))

  return 0


if __name__ == '__main__':
  sys.exit(main())
