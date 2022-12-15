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
import os
import re
import sys
import json

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--input', required=True)
  parser.add_argument('--output', required=True)
  args = parser.parse_args()

  with open(args.input, 'r') as f:
    modules = json.load(f)

  summary_objs = []
  summary_funs = []
  summary_view_funs = []
  s = []

  for module_name, module_files in modules.items():
    s.append(f'## Module: {module_name}')

    for file_dict in module_files:
      s.append(f'### Import: {file_dict["import_key"]}')

      # Add imports if in file.
      if file_dict['imports']:
        s.append('#### Views/Tables')
        for data in file_dict['imports']:
          # Anchor
          anchor = rf'obj/{module_name}/{data["name"]}'

          # Add summary of imported view/table
          summary_objs.append(f'[{data["name"]}](#{anchor})|'
                              f'{file_dict["import_key"]}|'
                              f'{data["desc"].split(".")[0]}')

          s.append(f'\n\n<a name="{anchor}"></a>'
                   f'**{data["name"]}**, {data["type"]}\n\n'
                   f'{data["desc"]}\n')

          s.append('Column | Description\n' '------ | -----------')
          for name, desc in data['cols'].items():
            s.append(f'{name} | {desc}')

      # Add functions if in file
      if file_dict['functions']:
        s.append('#### Functions')
        for data in file_dict['functions']:
          # Anchor
          anchor = rf'fun/{module_name}/{data["name"]}'

          # Add summary of imported function
          summary_funs.append(f'[{data["name"]}](#{anchor})|'
                              f'{file_dict["import_key"]}|'
                              f'{data["return_type"]}|'
                              f'{data["desc"].split(". ")[0]}')

          s.append(f'\n\n<a name="{anchor}"></a>'
                   f'**{data["name"]}**\n'
                   f'{data["desc"]}\n\n'
                   f'Returns: {data["return_type"]}, {data["return_desc"]}\n\n')

          s.append('Argument | Type | Description\n'
                   '-------- | ---- | -----------')
          for name, arg_dict in data['args'].items():
            s.append(f'{name} | {arg_dict["type"]} | {arg_dict["desc"]}')

      # Add view functions if in file
      if file_dict['view_functions']:
        s.append('#### View Functions')
        for data in file_dict['view_functions']:
          # Anchor
          anchor = rf'view_fun/{module_name}/{data["name"]}'
          # Add summary of imported view function
          summary_view_funs.append(f'[{data["name"]}](#{anchor})|'
                                   f'{file_dict["import_key"]}|'
                                   f'{data["desc"].split(". ")[0]}')

          s.append(f'\n\n<a name="{anchor}"></a>'
                   f'**{data["name"]}**\n'
                   f'{data["desc"]}\n\n')

          s.append('Argument | Type | Description\n'
                   '-------- | ---- | -----------')
          for name, arg_dict in data['args'].items():
            s.append(f'{name} | {arg_dict["type"]} | {arg_dict["desc"]}')
          s.append('\n')
          s.append('Column | Description\n' '------ | -----------')
          for name, desc in data['cols'].items():
            s.append(f'{name} | {desc}')

  with open(args.output, 'w+') as f:
    f.write("# SQL standard library\n"
            "To import any function, view_function, view or table simply run "
            "`SELECT IMPORT({import key});` in your SQL query.\n"
            "## Summary\n")

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
    f.write('\n'.join(s))

  return 0


if __name__ == '__main__':
  sys.exit(main())
