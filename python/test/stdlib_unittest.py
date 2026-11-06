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

import os
import sys
import unittest

ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.docs_parse import Arg, DocParseOptions, parse_module
from python.generators.sql_processing.utils import check_banned_usage


def _pfsql_module(**kwargs):
  """A module as printed by `pfsql parse`, with nothing in it by default."""
  module = {
      'module': 'slices.test',
      'path': 'slices/test.sql',
      'includes': [],
      'symbols': [],
      'references': [],
      'mutations': [],
      'uses_like': False,
      'docs': {
          'table_views': [],
          'functions': [],
          'macros': []
      },
      'errors': [],
  }
  module.update(kwargs)
  return module


def _pfsql_table(name='slices_table', description='Table.', **kwargs):
  table = {
      'name':
          name,
      'type':
          'TABLE',
      'description':
          description,
      'or_replace':
          False,
      'columns': [{
          'name': 'id',
          'type': 'JOINID(slice.id)',
          'description': 'Column.',
          'is_variadic': False,
      }],
  }
  table.update(kwargs)
  symbol = {'name': name, 'kind': 'table', 'form': 'perfetto', 'references': []}
  return _pfsql_module(
      symbols=[symbol],
      docs={
          'table_views': [table],
          'functions': [],
          'macros': []
      })


class TestStdlibPfsql(unittest.TestCase):

  def test_table(self):
    res = parse_module('slices/test.sql', _pfsql_table())
    self.assertListEqual(res.errors, [])
    self.assertEqual(res.table_views[0].name, 'slices_table')
    self.assertEqual(res.table_views[0].cols, {
        'id': Arg('JOINID', 'JOINID(slice.id)', 'Column.'),
    })

  def test_custom_module_prefix(self):
    self.assertListEqual(
        parse_module('chrome/util/test.sql', _pfsql_table('cr_table')).errors,
        [])
    self.assertListEqual(
        parse_module('chrome/util/test.sql',
                     _pfsql_table('chrome_table')).errors, [])
    self.assertEqual(
        len(
            parse_module('chrome/util/test.sql',
                         _pfsql_table('foo_table')).errors), 1)
    self.assertEqual(
        len(parse_module('foo/bar.sql', _pfsql_table('cr_table')).errors), 1)

  def test_validates_docs(self):
    res = parse_module('slices/test.sql',
                       _pfsql_table(description='', or_replace=True))
    self.assertEqual(len(res.errors), 1)
    self.assertIn('CREATE OR REPLACE is not allowed', res.errors[0])

  def test_function_and_macro_validation(self):
    module = _pfsql_module(
        docs={
            'table_views': [],
            'functions': [{
                'name': 'slices_SnakeCase',
                'description': 'Function.',
                'or_replace': False,
                'is_table_function': False,
                'return_type': 'BOOL',
                'return_description': '',
                'args': [],
                'columns': [],
            }],
            'macros': [{
                'name': 'slices_macro',
                'description': 'Macro.',
                'or_replace': True,
                'return_type': 'TableOrSubquery',
                'return_description': 'Exists.',
                'args': [],
            }],
        })
    res = parse_module('slices/test.sql', module)
    errors = '\n'.join(res.errors)
    self.assertIn('not snake_case', errors)
    self.assertIn('return description is missing', errors)
    self.assertIn('CREATE OR REPLACE is not allowed', errors)

  def test_deprecated_package_is_skipped(self):
    self.assertIsNone(parse_module('deprecated/test.sql', _pfsql_module()))

  def test_internal_virtual_table_is_kept(self):
    module = _pfsql_module(symbols=[{
        'name': '_span',
        'kind': 'table',
        'form': 'virtual',
        'references': ['a', 'b']
    }])
    res = parse_module('slices/test.sql', module,
                       DocParseOptions(include_internal=True))
    self.assertListEqual(res.errors, [])
    self.assertEqual(res.table_views[0].name, '_span')

  def test_banned_patterns(self):
    module = _pfsql_module(
        includes=['slices.*', 'viz.foo'],
        references=['RUN_METRIC'],
        uses_like=True,
        mutations=[{
            'kind': 'drop',
            'object_type': 'table',
            'name': 'gone'
        }, {
            'kind': 'insert',
            'object_type': '',
            'name': 't'
        }],
        symbols=[{
            'name': 'plain',
            'kind': 'table',
            'form': 'plain',
            'references': []
        }])
    parsed = parse_module('slices/test.sql', module,
                          DocParseOptions(include_internal=True))
    errors = '\n'.join(check_banned_usage(parsed, module))
    for expected in [
        'RUN_METRIC', 'LIKE', "Dropping object TABLE 'gone'", 'INSERT INTO',
        'wildcards', "'plain' uses CREATE TABLE", "depend on 'viz'"
    ]:
      self.assertIn(expected, errors)


if __name__ == '__main__':
  unittest.main()
