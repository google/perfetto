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

from python.generators.sql_processing.docs_parse import Arg as PfsqlArg
from python.generators.sql_processing.docs_parse import DocParseOptions, parse_module
from python.generators.sql_processing.docs_parse_legacy import Arg, parse_file
from python.generators.sql_processing import docs_parse, utils
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
        'id': PfsqlArg('JOINID', 'JOINID(slice.id)', 'Column.'),
    })

  def test_validates_docs(self):
    res = parse_module('slices/test.sql',
                       _pfsql_table(description='', or_replace=True))
    self.assertEqual(len(res.errors), 1)
    self.assertIn('CREATE OR REPLACE is not allowed', res.errors[0])

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


# Delete this along with docs_parse_legacy.py.
class TestStdlibLegacy(unittest.TestCase):

  # google3 calls these from their original modules.
  def test_google3_entry_points(self):
    sql = 'SELECT 1 FROM foo WHERE a LIKE b;'
    res = docs_parse.parse_file('slices/test.sql', sql)
    self.assertListEqual(res.errors, [])
    errors = utils.check_banned_patterns(res, sql)
    self.assertEqual(len(errors), 1)
    self.assertIn('LIKE is banned', errors[0])

  # Checks that custom prefixes (cr for chrome/util) are allowed.
  def test_custom_module_prefix(self):
    res = parse_file(
        'chrome/util/test.sql', f'''
-- Comment
CREATE PERFETTO TABLE cr_table(
    -- Column.
    x LONG
) AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.table_views[0]
    self.assertEqual(fn.name, 'cr_table')
    self.assertEqual(fn.desc, 'Comment')
    self.assertEqual(fn.cols, {
        'x': Arg('LONG', 'LONG', 'Column.'),
    })

  # Checks that when custom prefixes (cr for chrome/util) are present,
  # the full module name (chrome) is still accepted.
  def test_custom_module_prefix_full_module_name(self):
    res = parse_file(
        'chrome/util/test.sql', f'''
-- Comment
CREATE PERFETTO TABLE chrome_table(
    -- Column.
    x LONG
) AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.table_views[0]
    self.assertEqual(fn.name, 'chrome_table')
    self.assertEqual(fn.desc, 'Comment')
    self.assertEqual(fn.cols, {
        'x': Arg('LONG', 'LONG', 'Column.'),
    })

  # Checks that when custom prefixes (cr for chrome/util) are present,
  # the incorrect prefixes (foo) are not accepted.
  def test_custom_module_prefix_incorrect(self):
    res = parse_file(
        'chrome/util/test.sql', f'''
-- Comment
CREATE PERFETTO TABLE foo_table(
    -- Column.
    x LONG
) AS
SELECT 1;
    '''.strip())
    # Expecting an error: table prefix (foo) is not allowed for a given path
    # (allowed: chrome, cr).
    self.assertEqual(len(res.errors), 1)

  # Checks that when custom prefixes (cr for chrome/util) are present,
  # they do not apply outside of the path scope.
  def test_custom_module_prefix_does_not_apply_outside(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment
CREATE PERFETTO TABLE cr_table(
    -- Column.
    x LONG
) AS
SELECT 1;
    '''.strip())
    # Expecting an error: table prefix (foo) is not allowed for a given path
    # (allowed: foo).
    self.assertEqual(len(res.errors), 1)

  def test_ret_no_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment
CREATE PERFETTO FUNCTION foo_fn()
--
RETURNS BOOL
AS
SELECT TRUE;
    '''.strip())
    # Expecting an error: return value is missing a description.
    self.assertEqual(len(res.errors), 1)

  def test_multiline_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- This
-- is
--
-- a
--      very
--
-- long
--
-- description.
CREATE PERFETTO FUNCTION foo_fn()
-- Exists.
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.desc,
                     'This\n is\n\n a\n      very\n\n long\n\n description.')

  def test_function_name_style(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function comment.
CREATE PERFETTO FUNCTION foo_SnakeCase()
-- Exists.
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    # Expecting an error: function name should be using hacker_style.
    self.assertEqual(len(res.errors), 1)

  def test_table_with_schema(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Table comment.
CREATE PERFETTO TABLE foo_table(
    -- Id of slice.
    id LONG
) AS
SELECT 1 as id;
    '''.strip())
    self.assertListEqual(res.errors, [])

    table = res.table_views[0]
    self.assertEqual(table.name, 'foo_table')
    self.assertEqual(table.desc, 'Table comment.')
    self.assertEqual(table.type, 'TABLE')
    self.assertEqual(table.cols, {
        'id': Arg('LONG', 'LONG', 'Id of slice.'),
    })

  def test_perfetto_view_with_schema(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- View comment.
CREATE PERFETTO VIEW foo_table(
    -- Foo.
    foo LONG,
    -- Bar.
    bar STRING
) AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    table = res.table_views[0]
    self.assertEqual(table.name, 'foo_table')
    self.assertEqual(table.desc, 'View comment.')
    self.assertEqual(table.type, 'VIEW')
    self.assertEqual(table.cols, {
        'foo': Arg('LONG', 'LONG', 'Foo.'),
        'bar': Arg('STRING', 'STRING', 'Bar.'),
    })

  def test_function_with_new_style_docs(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE PERFETTO FUNCTION foo_fn(
    -- Utid of thread.
    utid LONG,
    -- String name.
    name STRING)
-- Exists.
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.name, 'foo_fn')
    self.assertEqual(fn.desc, 'Function foo.')
    self.assertEqual(
        fn.args, {
            'utid': Arg('LONG', 'LONG', 'Utid of thread.'),
            'name': Arg('STRING', 'STRING', 'String name.'),
        })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Exists.')

  def test_function_returns_table_with_new_style_docs(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE PERFETTO FUNCTION foo_fn(
    -- Utid of thread.
    utid LONG)
-- Impl comment.
RETURNS TABLE(
    -- Count.
    count LONG
)
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.table_functions[0]
    self.assertEqual(fn.name, 'foo_fn')
    self.assertEqual(fn.desc, 'Function foo.')
    self.assertEqual(fn.args, {
        'utid': Arg('LONG', 'LONG', 'Utid of thread.'),
    })
    self.assertEqual(fn.cols, {
        'count': Arg('LONG', 'LONG', 'Count.'),
    })

  def test_function_with_new_style_docs_multiline_comment(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE PERFETTO FUNCTION foo_fn(
    -- Multi
    -- line
    --
    -- comment.
    arg LONG)
-- Exists.
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.name, 'foo_fn')
    self.assertEqual(fn.desc, 'Function foo.')
    self.assertEqual(fn.args, {
        'arg': Arg('LONG', 'LONG', 'Multi line  comment.'),
    })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Exists.')

  def test_function_with_multiline_return_comment(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE PERFETTO FUNCTION foo_fn(
    -- Arg
    arg LONG)
-- Multi
-- line
-- return
-- comment.
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.name, 'foo_fn')
    self.assertEqual(fn.desc, 'Function foo.')
    self.assertEqual(fn.args, {
        'arg': Arg('LONG', 'LONG', 'Arg'),
    })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Multi line return comment.')

  def test_create_or_replace_table_banned(self):
    res = parse_file(
        'common/bar.sql', f'''
-- Table.
CREATE OR REPLACE PERFETTO TABLE foo(
    -- Column.
    x LONG
)
AS
SELECT 1;

    '''.strip())
    # Expecting an error: CREATE OR REPLACE is not allowed in stdlib.
    self.assertEqual(len(res.errors), 1)

  def test_create_or_replace_view_banned(self):
    res = parse_file(
        'common/bar.sql', f'''
-- Table.
CREATE OR REPLACE PERFETTO VIEW foo(
    -- Column.
    x LONG
)
AS
SELECT 1;

    '''.strip())
    # Expecting an error: CREATE OR REPLACE is not allowed in stdlib.
    self.assertEqual(len(res.errors), 1)

  def test_create_or_replace_function_banned(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE OR REPLACE PERFETTO FUNCTION foo_fn()
-- Exists.
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    # Expecting an error: CREATE OR REPLACE is not allowed in stdlib.
    self.assertEqual(len(res.errors), 1)

  def test_function_with_new_style_docs_with_parenthesis(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE PERFETTO FUNCTION foo_fn(
    -- Utid of thread (important).
    utid LONG)
-- Exists.
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.name, 'foo_fn')
    self.assertEqual(fn.desc, 'Function foo.')
    self.assertEqual(fn.args, {
        'utid': Arg('LONG', 'LONG', 'Utid of thread (important).'),
    })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Exists.')

  def test_macro(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Macro
CREATE OR REPLACE PERFETTO FUNCTION foo_fn()
-- Exists.
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    # Expecting an error: CREATE OR REPLACE is not allowed in stdlib.
    self.assertEqual(len(res.errors), 1)

  def test_create_or_replace_macro_smoke(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Macro
CREATE PERFETTO MACRO foo_macro(
  -- x Arg.
  x TableOrSubquery
)
-- Exists.
RETURNS TableOrSubquery
AS
SELECT 1;
    '''.strip())

    macro = res.macros[0]
    self.assertEqual(macro.name, 'foo_macro')
    self.assertEqual(macro.desc, 'Macro')
    self.assertEqual(macro.args, {
        'x': Arg('TableOrSubquery', 'TableOrSubquery', 'x Arg.'),
    })
    self.assertEqual(macro.return_type, 'TableOrSubquery')
    self.assertEqual(macro.return_desc, 'Exists.')

  def test_create_or_replace_macro_banned(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Macro
CREATE OR REPLACE PERFETTO MACRO foo_macro(
  -- x Arg.
  x TableOrSubquery
)
-- Exists.
RETURNS TableOrSubquery
AS
SELECT 1;
    '''.strip())
    # Expecting an error: CREATE OR REPLACE is not allowed in stdlib.
    self.assertEqual(len(res.errors), 1)
