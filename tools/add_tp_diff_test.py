#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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

import os
import pathlib
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def create_if_not_exists(path):
  create = not os.path.exists(path)
  if create:
    print('Creating empty file {}'.format(os.path.relpath(path, ROOT_DIR)))
    with open(path, 'a'):
      pass
  return create


def stdout_write(*args, **kwargs):
  sys.stdout.write(*args, **kwargs)
  sys.stdout.flush()


def main():
  test_dir = os.path.join(ROOT_DIR, 'test', 'trace_processor')
  include_index_path = os.path.join(test_dir, 'include_index')

  if not os.path.exists(include_index_path):
    print('Error: include index does not exist at {}'.format(
        os.path.relpath(include_index_path, ROOT_DIR)))
    return 1

  existing_folders = []
  with open(include_index_path, 'r') as include_index_file:
    for line in include_index_file.readlines():
      stripped = line.rstrip()
      existing_folders.append(stripped.replace('/index', ''))

  print('Pick a folder to add a test to. This can either be an existing '
        'folder or a new one to create.')
  print()
  print('Picking the correct folder is important to the long term health '
        'of trace processor. For help in this, please see the guidance at '
        'http://perfetto.dev/docs/analysis/trace-processor#diff-tests')
  print()
  print('Existing folders: {}.'.format(existing_folders))
  stdout_write('Folder: ')

  chosen_folder = sys.stdin.readline().rstrip()
  chosen_folder_path = os.path.abspath(os.path.join(test_dir, chosen_folder))
  chosen_folder_path_rel_root = os.path.relpath(chosen_folder_path, ROOT_DIR)
  if chosen_folder not in existing_folders:
    print('Creating new folder {} and adding include to include_index file'
          .format(chosen_folder))
    os.mkdir(chosen_folder_path)

    out_include_index = list(map(lambda x: x + '/index', existing_folders))
    out_include_index.append(chosen_folder + '/index')
    out_include_index.sort()

    with open(include_index_path, 'w') as include_index_file:
      include_index_file.write('\n'.join(out_include_index))
      include_index_file.write('\n')

  print()
  stdout_write('Pick the type of trace to be added [proto/textproto/python]: ')
  trace_type = sys.stdin.readline().rstrip()

  print()
  trace_file = ''
  if trace_type == 'proto':
    print('Proto traces should be added to the test_data GCS bucket '
          'using tools/test_data upload')
    stdout_write('Provide the name of the trace (including any '
                 'extension) relative to test/data: ')

    pb_file = sys.stdin.readline().rstrip()
    pb_path = os.path.abspath(os.path.join(ROOT_DIR, 'test', 'data', pb_file))
    if not os.path.exists(pb_path):
      print('Error: provided pb file {} does not exist',
            os.path.relpath(pb_path, ROOT_DIR))
      return 1

    trace_file = os.path.relpath(pb_path, chosen_folder_path)
  elif trace_type == 'textproto':
    print('Provide the path to the textproto trace relative to the '
          'chosen folder {}'.format(chosen_folder_path_rel_root))
    stdout_write(
        'If the file does not already exist, an empty file will be created: ')

    textproto_file = sys.stdin.readline().rstrip()
    textproto_path = os.path.abspath(
        os.path.join(chosen_folder_path, textproto_file))
    create_if_not_exists(textproto_path)

    trace_file = textproto_file
  elif trace_type == 'python':
    print(
        'Provide the path to the Python trace '
        'relative to the chosen folder {}'.format(chosen_folder_path_rel_root))
    stdout_write(
        'If the file does not already exist, an empty file will be created: ')

    python_file = sys.stdin.readline().rstrip()
    python_path = os.path.abspath(os.path.join(chosen_folder_path, python_file))
    if create_if_not_exists(python_path):
      print('For examples of how Python traces are constructed, '
            'check the existing traces in test/trace_processor')

    trace_file = python_file
  else:
    print('Error: unexpected trace type {}'.format(trace_type))
    return 1

  print()
  print(
      'Provide either the name of a built-in metric OR path to the file '
      '(which must end in "_test.sql") relative to the chosen folder {}'.format(
          chosen_folder_path_rel_root))
  stdout_write(
      'If the file does not already exist, an empty file will be created: ')

  sql_file_or_metric = sys.stdin.readline().rstrip()
  if sql_file_or_metric.endswith('.sql'):
    if not  sql_file_or_metric.endswith('_test.sql'):
      print('Error: SQL file {} must end in _test.sql'.format(sql_file_or_metric))
      return 1
    sql_path = os.path.abspath(
        os.path.join(chosen_folder_path, sql_file_or_metric))
    create_if_not_exists(sql_path)

  default_out_file = '{}_{}.out'.format(
      pathlib.Path(trace_file).stem,
      pathlib.Path(sql_file_or_metric).stem)

  print()
  print('Provide the name of the output file (or leave empty '
        'to accept the default: {})'.format(default_out_file))
  stdout_write(
      'If the file does not already exist, an empty file will be created: ')
  out_file = sys.stdin.readline().rstrip()
  if not out_file:
    out_file = default_out_file

  out_path = os.path.abspath(os.path.join(chosen_folder_path, out_file))
  create_if_not_exists(out_path)

  print()
  print('Appending test to index file')
  with open(os.path.join(chosen_folder_path, 'index'), 'a') as index_file:
    index_file.write('{} {} {}\n'.format(trace_file, sql_file_or_metric,
                                         out_file))

  index_rel_path = os.path.join(chosen_folder_path_rel_root, 'index')
  print()
  print(f'Please modify the index file at {index_rel_path} by adding a '
        f'comment and grouping with related tests')

  return 0


if __name__ == '__main__':
  sys.exit(main())
