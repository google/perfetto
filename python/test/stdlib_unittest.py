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

import unittest
import os
import sys

ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.append(os.path.join(ROOT_DIR))

from generators.stdlib_docs.stdlib import FunctionDocs, ViewFunctionDocs, TableViewDocs

DESC = """--
-- First line.
-- Second line."""

COLS_STR = """--
-- @column slice_id           Id of slice.
-- @column slice_name         Name of slice."""

COLS_SQL_STR = "slice_id INT, slice_name STRING"

ARGS_STR = """--
-- @arg utid INT              Utid of thread.
-- @arg name STRING           String name.
"""

ARGS_SQL_STR = "utid INT, name STRING"

RET_STR = """--
-- @ret BOOL                  Exists.
"""

RET_SQL_STR = "BOOL"

SQL_STR = "SELECT * FROM slice"


class TestStdlib(unittest.TestCase):

  # Valid schemas

  def test_valid_table(self):
    valid_table_comment = f"{DESC}\n{COLS_STR}".split('\n')

    docs, create_errors = TableViewDocs.create_from_comment(
        "", valid_table_comment, 'common', ('table', 'tab_name', 'to_ignore'))
    self.assertFalse(create_errors)

    validation_errors = docs.check_comment()
    self.assertFalse(validation_errors)

  def test_valid_function(self):
    valid_function = f"{DESC}\n{ARGS_STR}\n{RET_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, RET_SQL_STR, SQL_STR)
    docs, create_errors = FunctionDocs.create_from_comment(
        "", valid_function, 'common', valid_regex_matches)
    self.assertFalse(create_errors)

    validation_errors = docs.check_comment()
    self.assertFalse(validation_errors)

  def test_valid_view_function(self):
    valid_view_function = f"{DESC}\n{ARGS_STR}\n{COLS_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, COLS_SQL_STR, SQL_STR)
    docs, create_errors = ViewFunctionDocs.create_from_comment(
        "", valid_view_function, 'common', valid_regex_matches)
    self.assertFalse(create_errors)

    validation_errors = docs.check_comment()
    self.assertFalse(validation_errors)

  # Missing modules in names

  def test_missing_module_in_table_name(self):
    valid_table_comment = f"{DESC}\n{COLS_STR}".split('\n')

    _, create_errors = TableViewDocs.create_from_comment(
        "", valid_table_comment, 'android', ('table', 'tab_name', 'to_ignore'))
    self.assertTrue(create_errors)

  def test_missing_module_in_function_name(self):
    valid_function = f"{DESC}\n{ARGS_STR}\n{RET_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, RET_SQL_STR, SQL_STR)
    _, create_errors = FunctionDocs.create_from_comment("", valid_function,
                                                        'android',
                                                        valid_regex_matches)
    self.assertTrue(create_errors)

  def test_missing_module_in_view_function_name(self):
    valid_view_function = f"{DESC}\n{ARGS_STR}\n{COLS_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, COLS_SQL_STR, SQL_STR)
    _, create_errors = ViewFunctionDocs.create_from_comment(
        "", valid_view_function, 'android', valid_regex_matches)
    self.assertTrue(create_errors)

  # Missing part of schemas

  def test_missing_desc_in_table_name(self):
    comment = f"{COLS_STR}".split('\n')

    _, create_errors = TableViewDocs.create_from_comment(
        "", comment, 'common', ('table', 'tab_name', 'to_ignore'))
    self.assertTrue(create_errors)

  def test_missing_cols_in_table(self):
    comment = f"{DESC}".split('\n')

    _, create_errors = TableViewDocs.create_from_comment(
        "", comment, 'common', ('table', 'tab_name', 'to_ignore'))
    self.assertTrue(create_errors)

  def test_missing_desc_in_function(self):
    comment = f"{ARGS_STR}\n{RET_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, RET_SQL_STR, SQL_STR)
    _, create_errors = FunctionDocs.create_from_comment("", comment, 'common',
                                                        valid_regex_matches)
    self.assertTrue(create_errors)

  def test_missing_args_in_function(self):
    comment = f"{DESC}\n{RET_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, RET_SQL_STR, SQL_STR)
    _, create_errors = FunctionDocs.create_from_comment("", comment, 'common',
                                                        valid_regex_matches)
    self.assertTrue(create_errors)

  def test_missing_ret_in_function(self):
    comment = f"{DESC}\n{ARGS_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, RET_SQL_STR, SQL_STR)
    _, create_errors = FunctionDocs.create_from_comment("", comment, 'common',
                                                        valid_regex_matches)
    self.assertTrue(create_errors)

  def test_missing_desc_in_view_function(self):
    comment = f"{ARGS_STR}\n{COLS_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, COLS_SQL_STR, SQL_STR)
    _, create_errors = ViewFunctionDocs.create_from_comment(
        "", comment, 'common', valid_regex_matches)
    self.assertTrue(create_errors)

  def test_missing_args_in_view_function(self):
    comment = f"{DESC}\n{COLS_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, COLS_SQL_STR, SQL_STR)
    _, create_errors = ViewFunctionDocs.create_from_comment(
        "", comment, 'common', valid_regex_matches)
    self.assertTrue(create_errors)

  def test_missing_cols_in_view_function(self):
    comment = f"{DESC}\n{ARGS_STR}".split('\n')
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, COLS_SQL_STR, SQL_STR)
    _, create_errors = ViewFunctionDocs.create_from_comment(
        "", comment, 'common', valid_regex_matches)
    self.assertTrue(create_errors)

  # Validate elements

  def test_invalid_table_columns(self):
    invalid_cols = "-- @column slice_id"
    comment = f"{DESC}\n{invalid_cols}".split('\n')

    docs, create_errors = TableViewDocs.create_from_comment(
        "", comment, 'common', ('table', 'tab_name', 'to_ignore'))
    self.assertFalse(create_errors)

    validation_errors = docs.check_comment()
    self.assertTrue(validation_errors)

  def test_invalid_view_function_columns(self):
    comment = f"{DESC}\n{ARGS_STR}\n{COLS_STR}".split('\n')
    cols_sql_str = "slice_id INT"
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, cols_sql_str, SQL_STR)
    docs, create_errors = ViewFunctionDocs.create_from_comment(
        "", comment, 'common', valid_regex_matches)
    self.assertFalse(create_errors)

    validation_errors = docs.check_comment()
    self.assertTrue(validation_errors)

  def test_invalid_arguments(self):
    valid_function = f"{DESC}\n{ARGS_STR}\n{RET_STR}".split('\n')
    args_sql_str = "utid BOOL"
    valid_regex_matches = ('fun_name', args_sql_str, RET_SQL_STR, SQL_STR)
    docs, create_errors = FunctionDocs.create_from_comment(
        "", valid_function, 'common', valid_regex_matches)
    self.assertFalse(create_errors)

    validation_errors = docs.check_comment()
    self.assertTrue(validation_errors)

  def test_invalid_ret(self):
    valid_function = f"{DESC}\n{ARGS_STR}\n{RET_STR}".split('\n')
    ret_sql_str = "utid BOOL"
    valid_regex_matches = ('fun_name', ARGS_SQL_STR, ret_sql_str, SQL_STR)
    docs, create_errors = FunctionDocs.create_from_comment(
        "", valid_function, 'common', valid_regex_matches)
    self.assertFalse(create_errors)

    validation_errors = docs.check_comment()
    self.assertTrue(validation_errors)
