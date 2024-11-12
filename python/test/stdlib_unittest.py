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

from python.generators.sql_processing.docs_parse import Arg, parse_file


class TestStdlib(unittest.TestCase):

  # Checks that custom prefixes (cr for chrome/util) are allowed.
  def test_custom_module_prefix(self):
    res = parse_file(
        'chrome/util/test.sql', f'''
-- Comment
CREATE PERFETTO TABLE cr_table(
    -- Column.
    x INT
) AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.table_views[0]
    self.assertEqual(fn.name, 'cr_table')
    self.assertEqual(fn.desc, 'Comment')
    self.assertEqual(fn.cols, {
        'x': Arg('INT', 'Column.'),
    })

  # Checks that when custom prefixes (cr for chrome/util) are present,
  # the full module name (chrome) is still accepted.
  def test_custom_module_prefix_full_module_name(self):
    res = parse_file(
        'chrome/util/test.sql', f'''
-- Comment
CREATE PERFETTO TABLE chrome_table(
    -- Column.
    x INT
) AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.table_views[0]
    self.assertEqual(fn.name, 'chrome_table')
    self.assertEqual(fn.desc, 'Comment')
    self.assertEqual(fn.cols, {
        'x': Arg('INT', 'Column.'),
    })

  # Checks that when custom prefixes (cr for chrome/util) are present,
  # the incorrect prefixes (foo) are not accepted.
  def test_custom_module_prefix_incorrect(self):
    res = parse_file(
        'chrome/util/test.sql', f'''
-- Comment
CREATE PERFETTO TABLE foo_table(
    -- Column.
    x INT
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
    x INT
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
    id INT
) AS
SELECT 1 as id;
    '''.strip())
    self.assertListEqual(res.errors, [])

    table = res.table_views[0]
    self.assertEqual(table.name, 'foo_table')
    self.assertEqual(table.desc, 'Table comment.')
    self.assertEqual(table.type, 'TABLE')
    self.assertEqual(table.cols, {
        'id': Arg('INT', 'Id of slice.'),
    })

  def test_perfetto_view_with_schema(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- View comment.
CREATE PERFETTO VIEW foo_table(
    -- Foo.
    foo INT,
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
        'foo': Arg('INT', 'Foo.'),
        'bar': Arg('STRING', 'Bar.'),
    })

  def test_function_with_new_style_docs(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE PERFETTO FUNCTION foo_fn(
    -- Utid of thread.
    utid INT,
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
            'utid': Arg('INT', 'Utid of thread.'),
            'name': Arg('STRING', 'String name.'),
        })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Exists.')

  def test_function_returns_table_with_new_style_docs(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE PERFETTO FUNCTION foo_fn(
    -- Utid of thread.
    utid INT)
-- Impl comment.
RETURNS TABLE(
    -- Count.
    count INT
)
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.table_functions[0]
    self.assertEqual(fn.name, 'foo_fn')
    self.assertEqual(fn.desc, 'Function foo.')
    self.assertEqual(fn.args, {
        'utid': Arg('INT', 'Utid of thread.'),
    })
    self.assertEqual(fn.cols, {
        'count': Arg('INT', 'Count.'),
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
    arg INT)
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
        'arg': Arg('INT', 'Multi line  comment.'),
    })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Exists.')

  def test_function_with_multiline_return_comment(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
CREATE PERFETTO FUNCTION foo_fn(
    -- Arg
    arg INT)
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
        'arg': Arg('INT', 'Arg'),
    })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Multi line return comment.')

  def test_create_or_replace_table_banned(self):
    res = parse_file(
        'common/bar.sql', f'''
-- Table.
CREATE OR REPLACE PERFETTO TABLE foo(
    -- Column.
    x INT
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
    x INT
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
    utid INT)
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
        'utid': Arg('INT', 'Utid of thread (important).'),
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
        'x': Arg('TableOrSubquery', 'x Arg.'),
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
