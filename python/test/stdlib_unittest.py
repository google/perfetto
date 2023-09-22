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

from python.generators.sql_processing.docs_parse import parse_file

DESC = """--
-- First line.
-- Second line."""

COLS_STR = """--
-- @column slice_id           Id of slice.
-- @column slice_name         Name of slice."""

COLS_SQL_STR = "slice_id INT, slice_name STRING"

ARGS_STR = """--
-- @arg utid INT              Utid of thread.
-- @arg name STRING           String name."""

ARGS_SQL_STR = "utid INT, name STRING"

RET_STR = """--
-- @ret BOOL                  Exists."""

RET_SQL_STR = "BOOL"

SQL_STR = "SELECT * FROM slice"


class TestStdlib(unittest.TestCase):

  def test_valid_table(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
{COLS_STR}
CREATE TABLE foo_table AS
{SQL_STR};
    '''.strip())
    self.assertListEqual(res.errors, [])

    table = res.table_views[0]
    self.assertEqual(table.name, 'foo_table')
    self.assertEqual(table.desc, 'First line. Second line.')
    self.assertEqual(table.type, 'TABLE')
    self.assertEqual(table.cols, {
        'slice_id': 'Id of slice.',
        'slice_name': 'Name of slice.'
    })

  def test_valid_function(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
{ARGS_STR}
{RET_STR}
CREATE PERFETTO FUNCTION foo_fn({ARGS_SQL_STR})
RETURNS {RET_SQL_STR}
AS
{SQL_STR};
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.name, 'foo_fn')
    self.assertEqual(fn.desc, 'First line. Second line.')
    self.assertEqual(
        fn.args, {
            'utid': {
                'type': 'INT',
                'desc': 'Utid of thread.',
            },
            'name': {
                'type': 'STRING',
                'desc': 'String name.',
            },
        })
    self.assertEqual(fn.return_type, 'BOOL')
    self.assertEqual(fn.return_desc, 'Exists.')

  def test_valid_view_function(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
{ARGS_STR}
{COLS_STR}
SELECT CREATE_VIEW_FUNCTION(
  'foo_view_fn({ARGS_SQL_STR})',
  '{COLS_SQL_STR}',
  '{SQL_STR}'
);
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.table_functions[0]
    self.assertEqual(fn.name, 'foo_view_fn')
    self.assertEqual(fn.desc, 'First line. Second line.')
    self.assertEqual(
        fn.args, {
            'utid': {
                'type': 'INT',
                'desc': 'Utid of thread.',
            },
            'name': {
                'type': 'STRING',
                'desc': 'String name.',
            },
        })
    self.assertEqual(fn.cols, {
        'slice_id': 'Id of slice.',
        'slice_name': 'Name of slice.'
    })

  def test_missing_module_name(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
{COLS_STR}
CREATE TABLE bar_table AS
{SQL_STR};
    '''.strip())
    # Expecting an error: function prefix (bar) not matching module name (foo).
    self.assertEqual(len(res.errors), 1)

  def test_common_does_not_include_module_name(self):
    res = parse_file(
        'common/bar.sql', f'''
{DESC}
{COLS_STR}
CREATE TABLE common_table AS
{SQL_STR};
    '''.strip())
    # Expecting an error: functions in common/ should not have a module prefix.
    self.assertEqual(len(res.errors), 1)

  def test_cols_typo(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
--
-- @column slice_id2          Foo.
-- @column slice_name         Bar.
CREATE TABLE bar_table AS
{SQL_STR};
    '''.strip())
    # Expecting an error: column slice_id2 not found in the table.
    self.assertEqual(len(res.errors), 1)

  def test_cols_no_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
--
-- @column slice_id
-- @column slice_name         Bar.
CREATE TABLE bar_table AS
{SQL_STR};
    '''.strip())
    # Expecting an error: column slice_id is missing a description.
    self.assertEqual(len(res.errors), 1)

  def test_args_typo(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
--
-- @arg utid2 INT             Uint.
-- @arg name STRING           String name.
{RET_STR}
CREATE PERFETTO FUNCTION foo_fn({ARGS_SQL_STR})
RETURNS {RET_SQL_STR}
AS
{SQL_STR};
    '''.strip())
    # Expecting 2 errors:
    # - arg utid2 not found in the function (should be utid);
    # - utid not documented.
    self.assertEqual(len(res.errors), 2)

  def test_args_no_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
--
-- @arg utid INT
-- @arg name STRING           String name.
{RET_STR}
CREATE PERFETTO FUNCTION foo_fn({ARGS_SQL_STR})
RETURNS {RET_SQL_STR}
AS
{SQL_STR};
    '''.strip())
    # Expecting 2 errors:
    # - arg utid is missing a description;
    # - arg utid is not documented.
    self.assertEqual(len(res.errors), 2)

  def test_ret_no_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
{ARGS_STR}
--
-- @ret BOOL
CREATE PERFETTO FUNCTION foo_fn({ARGS_SQL_STR})
RETURNS {RET_SQL_STR}
AS
{SQL_STR};
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
{ARGS_STR}
{RET_STR}
CREATE PERFETTO FUNCTION foo_fn({ARGS_SQL_STR})
RETURNS {RET_SQL_STR}
AS
{SQL_STR};
    '''.strip())
    self.assertListEqual(res.errors, [])

    fn = res.functions[0]
    self.assertEqual(fn.desc, 'This is a very long description.')

  def test_multiline_arg_desc(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
--
-- @arg utid INT              Uint
-- spread
--
-- across lines.
-- @arg name STRING            String name
--                             which spans across multiple lines
-- inconsistently.
{RET_STR}
CREATE PERFETTO FUNCTION foo_fn({ARGS_SQL_STR})
RETURNS {RET_SQL_STR}
AS
{SQL_STR};
    '''.strip())

    fn = res.functions[0]
    self.assertEqual(
        fn.args, {
            'utid': {
                'type': 'INT',
                'desc': 'Uint spread across lines.',
            },
            'name': {
                'type': 'STRING',
                'desc': 'String name which spans across multiple lines '
                        'inconsistently.',
            },
        })

  def test_function_name_style(self):
    res = parse_file(
        'foo/bar.sql', f'''
{DESC}
{ARGS_STR}
{RET_STR}
CREATE PERFETTO FUNCTION foo_SnakeCase({ARGS_SQL_STR})
RETURNS {RET_SQL_STR}
AS
{SQL_STR};
    '''.strip())
    # Expecting an error: function name should be using hacker_style.
    self.assertEqual(len(res.errors), 1)