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

  def test_valid_table(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- First line.
-- Second line.
-- @column slice_id           Id of slice.
-- @column slice_name         Name of slice.
CREATE TABLE foo_table AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    table = res.table_views[0]
    self.assertEqual(table.name, 'foo_table')
    self.assertEqual(table.desc, 'First line. Second line.')
    self.assertEqual(table.type, 'TABLE')
    self.assertEqual(
        table.cols, {
            'slice_id': Arg(None, 'Id of slice.'),
            'slice_name': Arg(None, 'Name of slice.'),
        })

  def test_valid_function(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- First line.
-- Second line.
-- @arg utid INT              Utid of thread.
-- @arg name STRING           String name.
-- @ret BOOL Exists.
CREATE PERFETTO FUNCTION foo_fn(utid INT, name STRING)
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.name, 'foo_fn')
    self.assertEqual(fn.desc, 'First line. Second line.')
    self.assertEqual(
        fn.args, {
            'utid': Arg('INT', 'Utid of thread.'),
            'name': Arg('STRING', 'String name.'),
        })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Exists.')

  def test_valid_table_function(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Table comment.
-- @arg utid INT              Utid of thread.
-- @arg name STRING           String name.
-- @column slice_id           Id of slice.
-- @column slice_name         Name of slice.
CREATE PERFETTO FUNCTION foo_view_fn(utid INT, name STRING)
RETURNS TABLE(slice_id INT, slice_name STRING)
AS SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.table_functions[0]
    self.assertEqual(fn.name, 'foo_view_fn')
    self.assertEqual(fn.desc, 'Table comment.')
    self.assertEqual(
        fn.args, {
            'utid': Arg('INT', 'Utid of thread.'),
            'name': Arg('STRING', 'String name.'),
        })
    self.assertEqual(
        fn.cols, {
            'slice_id': Arg('INT', 'Id of slice.'),
            'slice_name': Arg('STRING', 'Name of slice.'),
        })

  def test_missing_module_name(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment
-- @column slice_id           Id of slice.
CREATE TABLE bar_table AS
SELECT 1;
    '''.strip())
    # Expecting an error: function prefix (bar) not matching module name (foo).
    self.assertEqual(len(res.errors), 1)

  def test_common_does_not_include_module_name(self):
    res = parse_file(
        'common/bar.sql', f'''
-- Comment.
-- @column slice_id           Id of slice.
CREATE TABLE common_table AS
SELECT 1;
    '''.strip())
    # Expecting an error: functions in common/ should not have a module prefix.
    self.assertEqual(len(res.errors), 1)

  def test_cols_typo(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment.
--
-- @column slice_id2          Foo.
-- @column slice_name         Bar.
CREATE TABLE bar_table AS
SELECT 1;
    '''.strip())
    # Expecting an error: column slice_id2 not found in the table.
    self.assertEqual(len(res.errors), 1)

  def test_cols_no_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment.
--
-- @column slice_id
-- @column slice_name         Bar.
CREATE TABLE bar_table AS
SELECT 1;
    '''.strip())
    # Expecting an error: column slice_id is missing a description.
    self.assertEqual(len(res.errors), 1)

  def test_args_typo(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment.
--
-- @arg utid2 INT             Uint.
-- @arg name STRING           String name.
-- @ret BOOL                  Exists.
CREATE PERFETTO FUNCTION foo_fn(utid INT, name STRING)
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    # Expecting 2 errors:
    # - arg utid2 not found in the function (should be utid);
    # - utid not documented.
    self.assertEqual(len(res.errors), 2)

  def test_args_no_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment.
--
-- @arg utid INT
-- @arg name STRING           String name.
-- @ret BOOL                  Exists.
CREATE PERFETTO FUNCTION foo_fn(utid INT, name STRING)
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    # Expecting 2 errors:
    # - arg utid is missing a description;
    # - arg utid is not documented.
    self.assertEqual(len(res.errors), 2)

  def test_ret_no_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment
--
-- @ret BOOL
CREATE PERFETTO FUNCTION foo_fn()
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
-- @ret BOOL                  Exists.
CREATE PERFETTO FUNCTION foo_fn()
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.desc, 'This is a very long description.')

  def test_multiline_arg_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Comment.
--
-- @arg utid INT              Uint
-- spread
--
-- across lines.
-- @arg name STRING            String name
--                             which spans across multiple lines
-- inconsistently.
-- @ret BOOL                  Exists.
CREATE PERFETTO FUNCTION foo_fn(utid INT, name STRING)
RETURNS BOOL
AS
SELECT 1;
    '''.strip())

    fn = res.functions[0]
    self.assertEqual(
        fn.args, {
            'utid':
                Arg('INT', 'Uint spread across lines.'),
            'name':
                Arg(
                    'STRING', 'String name which spans across multiple lines '
                    'inconsistently.'),
        })

  def test_function_name_style(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function comment.
-- @ret BOOL                  Exists.
CREATE PERFETTO FUNCTION foo_SnakeCase()
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
-- @ret BOOL                  Exists.
CREATE PERFETTO FUNCTION foo_fn(
    -- Utid of thread.
    utid INT,
    -- String name.
    name STRING)
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

  def test_function_with_new_style_docs_multiline_comment(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
-- @ret BOOL                  Exists.
CREATE PERFETTO FUNCTION foo_fn(
    -- Multi
    -- line
    --
    -- comment.
    arg INT)
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

  def test_create_or_replace_table_banned(self):
    res = parse_file(
        'common/bar.sql', f'''
-- Table.
CREATE OR REPLACE PERFETTO TABLE foo(
    -- Column.
    x INT,
)
RETURNS BOOL
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
    x INT,
)
RETURNS BOOL
AS
SELECT 1;

    '''.strip())
    # Expecting an error: CREATE OR REPLACE is not allowed in stdlib.
    self.assertEqual(len(res.errors), 1)

  def test_create_or_replace_function_banned(self):
    res = parse_file(
        'foo/bar.sql', f'''
-- Function foo.
-- @ret BOOL                  Exists.
CREATE OR REPLACE PERFETTO FUNCTION foo_fn()
RETURNS BOOL
AS
SELECT 1;
    '''.strip())
    # Expecting an error: CREATE OR REPLACE is not allowed in stdlib.
    self.assertEqual(len(res.errors), 1)
