#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

import sys
from pathlib import Path
import argparse
import re
from typing import Dict, List, Tuple, Union
import typing as t

try:
  import sqlglot
  from sqlglot.dialects.dialect import rename_func
  from sqlglot import exp
  from sqlglot.dialects.sqlite import SQLite
  from sqlglot.tokens import TokenType
except ModuleNotFoundError as e:
  print('Failed to import sqlglot. Run tools/install-build-deps first.')
  print('If the error persist, make sure you are using the .venv')
  raise e


class Perfetto(SQLite):
  """Perfetto SQL dialect implementation."""

  class Generator(SQLite.Generator):
    """Generator for Perfetto SQL dialect."""

    CREATABLE_KIND_MAPPING = {
        "PERFETTO INDEX": "INDEX",
    }

    TYPE_MAPPING = {
        **SQLite.Generator.TYPE_MAPPING,
        exp.DataType.Type.BOOLEAN: "BOOL",
        exp.DataType.Type.BIGINT: "LONG",
        exp.DataType.Type.TEXT: "STRING",
        exp.DataType.Type.DOUBLE: "DOUBLE",
    }

    TRANSFORMS = {
        **SQLite.Generator.TRANSFORMS,
        exp.NEQ:
            lambda self, e: self.binary(e, "!="),
        exp.Substring:
            rename_func("SUBSTR"),
        exp.ReturnsProperty:
            lambda self, e: self.return_property(e),
    }

    PROPERTIES_LOCATION = {
        **SQLite.Generator.PROPERTIES_LOCATION,
        exp.ReturnsProperty:
            exp.Properties.Location.POST_SCHEMA,
    }

    SUPPORTS_TABLE_ALIAS_COLUMNS = True

    JSON_KEY_VALUE_PAIR_SEP = ","

    def maybe_comment(self,
                      sql: str,
                      expression: t.Optional[exp.Expression] = None,
                      comments: t.Optional[t.List[str]] = None,
                      separated: bool = False):
      comments = (((expression and expression.comments) if comments is None else
                   comments) if self.comments else None)
      if not comments or isinstance(expression, exp.Connector):
        return sql

      comments_sql = "\n".join(f"--{comment.rstrip()}" for comment in comments)
      if not comments_sql:
        return sql
      sep = ('\n' if sql and sql[0].isspace() else '') + comments_sql
      return f"{sep}{self.sep()}{sql.strip()}"

    def expressions(
        self,
        expression: t.Optional[exp.Expression] = None,
        key: t.Optional[str] = None,
        sqls: t.Optional[t.Collection[t.Union[str, exp.Expression]]] = None,
        flat: bool = False,
        indent: bool = True,
        skip_first: bool = False,
        skip_last: bool = False,
        sep: str = ", ",
        prefix: str = "",
        dynamic: bool = False,
        new_line: bool = False,
    ) -> str:
      expressions = expression.args.get(key or
                                        "expressions") if expression else sqls

      if not expressions:
        return ""

      if flat:
        return sep.join(
            sql for sql in (self.sql(e) for e in expressions) if sql)

      num_sqls = len(expressions)
      result_sqls = []

      for i, e in enumerate(expressions):
        sql = self.sql(e, comment=False)
        if not sql:
          continue

        comments = self.maybe_comment("", e) if isinstance(
            e, exp.Expression) else ""

        if self.pretty:
          if self.leading_comma:
            result_sqls.append(f"{sep if i > 0 else ''}{prefix}{comments}{sql}")
          else:
            result_sqls.append(
                f"{prefix}{comments}{sql}{(sep.rstrip() if comments else sep) if i + 1 < num_sqls else ''}"
            )
        else:
          result_sqls.append(
              f"{prefix}{comments}{sql}{sep if i + 1 < num_sqls else ''}")

      if self.pretty and (not dynamic or self.too_wide(result_sqls)):
        if new_line:
          result_sqls.insert(0, "")
          result_sqls.append("")
        result_sql = "\n".join(s.rstrip() for s in result_sqls)
      else:
        result_sql = "".join(result_sqls)

      return (self.indent(
          result_sql, skip_first=skip_first, skip_last=skip_last)
              if indent else result_sql)

    def not_sql(self, expression: exp.Not) -> str:
      if isinstance(expression.this, exp.Is) and isinstance(
          expression.this.right, exp.Null):
        return f"{self.sql(expression.this.left)} IS NOT NULL"
      return super().not_sql(expression)

    def connector_sql(
        self,
        expression: exp.Connector,
        op: str,
        stack: t.Optional[t.List[t.Union[str, exp.Expression]]] = None,
    ) -> str:
      if stack is not None:
        if expression.expressions:
          stack.append(self.expressions(expression, sep=f" {op} "))
        else:
          if expression.comments and self.comments:
            comments = []
            for comment in expression.comments:
              if comment:
                comments.append(f"--{self.pad_comment(comment).rstrip()}")
            op = "\n".join(comments) + "\n" + op
          stack.extend((expression.right, op, expression.left))
        return op
      return super().connector_sql(expression, op, stack)

    def case_sql(self, expression: exp.Case) -> str:
      this = self.sql(expression, "this")
      statements = [f"CASE {this}" if this else "CASE"]

      for e in expression.args["ifs"]:
        statements.append(self.maybe_comment(f"WHEN {self.sql(e, 'this')}", e))
        statements.append(f"THEN {self.sql(e, 'true')}")

      default = self.sql(expression, "default")

      if default:
        statements.append(f"ELSE {default}")

      statements.append("END")

      if self.pretty and self.too_wide(statements):
        return self.indent(
            "\n".join(statements), skip_first=True, skip_last=True)

      return " ".join(statements)

    def with_sql(self, expression: exp.With) -> str:
      sql = self.expressions(expression)
      recursive = ("RECURSIVE " if self.CTE_RECURSIVE_KEYWORD_REQUIRED and
                   expression.args.get("recursive") else "")
      return f"WITH\n{recursive}{sql}"

    def return_property(self, expression: exp.ReturnsProperty) -> str:
      return f"RETURNS {self.sql(expression, 'this')}"

    # https://www.sqlite.org/lang_aggfunc.html#group_concat
    def groupconcat_sql(self, expression: exp.GroupConcat) -> str:
      this = expression.this
      distinct = expression.find(exp.Distinct)

      if distinct:
        this = distinct.expressions[0]
        distinct_sql = "DISTINCT "
      else:
        distinct_sql = ""

      separator = expression.args.get("separator")
      return f"GROUP_CONCAT({distinct_sql}{self.format_args(this, separator)})"

  class Parser(SQLite.Parser):
    STATEMENT_PARSERS = {
        **SQLite.Parser.STATEMENT_PARSERS,
        TokenType.CREATE:
            lambda self: self._parse_create_override(),
        TokenType.VAR:
            lambda self: self._parse_var_override(),
    }

    def _parse_create_override(self):
      if self._match_text_seq("VIRTUAL"):
        return self._parse_create_virtual_table_override()

      if not self._match_text_seq("PERFETTO"):
        return self._parse_create()

      is_table = self._match_text_seq("TABLE")
      is_view = self._match_text_seq("VIEW")
      is_function = self._match_text_seq("FUNCTION")
      is_index = self._match_text_seq("INDEX")
      is_macro = self._match_text_seq("MACRO")
      if not is_table and not is_view and not is_function and not is_index and not is_macro:
        return self.raise_error(
            "Expected 'TABLE', 'VIEW', 'FUNCTION', 'INDEX' or 'MACRO'")

      if is_index:
        # Parse index name
        name = self._parse_id_var()

        # Parse ON
        if not self._match_text_seq("ON"):
          return self.raise_error("Expected 'ON'")

        # Parse table name
        table = self._parse_table()

        return exp.Create(
            this=self.expression(
                exp.Index,
                this=name,
                table=table,
            ),
            kind='PERFETTO INDEX',
        )

      if is_function or is_macro:
        # Parse function name
        udf = self._parse_user_defined_function()

        # Parse RETURNS type
        if not self._match_text_seq("RETURNS"):
          return self.raise_error("Expected 'RETURNS'")
        return_comments = self._prev_comments
        return_type = self._parse_returns()
        return_type.comments = return_comments

        # Parse AS
        if not self._match(TokenType.ALIAS):
          return self.raise_error("Expected 'AS'")

        if is_function:
          # Parse function body
          body = self._parse_select()
        else:
          if str(return_type.this).upper() == "_PROJECTIONFRAGMENT":
            body = self._parse_projections()
          else:
            body = self._parse_expression()

        return exp.Create(
            this=udf,
            kind="PERFETTO FUNCTION" if is_function else "PERFETTO MACRO",
            expression=body,
            properties=self.expression(
                exp.Properties,
                expressions=[
                    return_type,
                ],
            ),
        )

      # Parse view/table name
      table = self._parse_table(schema=True)

      # Parse AS
      if not self._match(TokenType.ALIAS):
        return self.raise_error("Expected 'AS'")

      # Parse SELECT statement
      select = self._parse_select()

      return exp.Create(
          this=table,
          kind='PERFETTO VIEW' if is_view else 'PERFETTO TABLE',
          expression=select)

    def _parse_var_override(self):
      assert self._prev
      if self._prev.text.upper() != "INCLUDE":
        return self.raise_error("Expected 'INCLUDE'")

      # Expect 'PERFETTO'
      if not self._match_text_seq('PERFETTO'):
        return self.raise_error("Expected 'PERFETTO'")

      # Expect 'MODULE'
      if not self._match_text_seq('MODULE'):
        return self.raise_error("Expected 'MODULE'")

      # Parse the module path (e.g. android.suspend)
      module_path = []
      while True:
        id = self._parse_id_var()
        if not id:
          break
        module_path.append(id.text(key="this"))
        if not self._match(TokenType.DOT):
          break
      return exp.Command(this="INCLUDE PERFETTO MODULE " +
                         '.'.join(module_path))

    def _parse_create_virtual_table_override(self):
      if not self._match(TokenType.TABLE):
        return self.raise_error("Expected 'TABLE'")

      name = self._parse_id_var()

      if not self._match_text_seq('USING'):
        return self.raise_error("Expected 'USING'")

      sp = self._parse_id_var()
      self._match_l_paren()
      start = self._prev

      while not self._match(TokenType.R_PAREN):
        self._advance()

      return exp.Command(
          this="CREATE VIRTUAL TABLE " + name.text(key="this") + " USING " +
          sp.text(key="this") + " " + self._find_sql(start, self._prev),)

    def _parse_case(self) -> t.Optional[exp.Expression]:
      ifs = []
      default = None

      comments = self._prev_comments
      expression = self._parse_assignment()

      while self._match(TokenType.WHEN):
        when_comments = self._prev_comments
        this = self._parse_assignment()
        self._match(TokenType.THEN)
        then = self._parse_assignment()
        ifs.append(
            self.expression(
                exp.If, this=this, true=then, comments=when_comments))

      if self._match(TokenType.ELSE):
        default = self._parse_assignment()

      if not self._match(TokenType.END):
        if isinstance(default,
                      exp.Interval) and default.this.sql().upper() == "END":
          default = exp.column("interval")
        else:
          self.raise_error("Expected END after CASE", self._prev)

      return self.expression(
          exp.Case,
          comments=comments,
          this=expression,
          ifs=ifs,
          default=default)

    def _parse_is(
        self, this: t.Optional[exp.Expression]) -> t.Optional[exp.Expression]:
      index = self._index - 1
      negate = self._match(TokenType.NOT)

      if self._match_text_seq("DISTINCT", "FROM"):
        klass = exp.NullSafeEQ if negate else exp.NullSafeNEQ
        return self.expression(
            klass, this=this, expression=self._parse_bitwise())

      if self._match(TokenType.JSON):
        kind = self._match_texts(
            self.IS_JSON_PREDICATE_KIND) and self._prev.text.upper()

        if self._match_text_seq("WITH"):
          _with = True
        elif self._match_text_seq("WITHOUT"):
          _with = False
        else:
          _with = None

        unique = self._match(TokenType.UNIQUE)
        self._match_text_seq("KEYS")
        expression: t.Optional[exp.Expression] = self.expression(
            exp.JSON, **{
                "this": kind,
                "with": _with,
                "unique": unique
            })
      else:
        expression = self._parse_expression()
        if not expression:
          self._retreat(index)
          return None

      this = self.expression(exp.Is, this=this, expression=expression)
      return self.expression(exp.Not, this=this) if negate else this


def preprocess_macros(sql: str) -> Tuple[str, List[Tuple[str, str]]]:
  """Convert macro calls to placeholders for sqlglot parsing.

  Args:
    sql: Input SQL string

  Returns:
    Tuple of (processed SQL, list of (placeholder, original macro text) pairs)
  """
  result = sql
  macros = []

  for match in re.finditer(r'(\w+)\s*!\s*\(', sql):
    start = match.start()
    i = match.end()
    paren = 1
    while i < len(sql) and paren > 0:
      if sql[i] == '(':
        paren += 1
      elif sql[i] == ')':
        paren -= 1
      i += 1

    if paren == 0:
      macro = sql[start:i]
      placeholder = f'__macro_{len(macros)}__'
      macros.append((placeholder, macro))
      result = result.replace(macro, placeholder)

  return result, macros


def postprocess_macros(sql: str, macros: List[Tuple[str, str]]) -> str:
  """Restore macro calls from their placeholders.

  Args:
    sql: Formatted SQL with placeholders
    macros: List of (placeholder, original macro text) pairs from preprocess_macros

  Returns:
    SQL with macros restored
  """
  result = sql
  for placeholder, macro_text in macros:
    result = result.replace(placeholder, macro_text)
  return result


def extract_comment_blocks(sql: str) -> Tuple[str, Dict[str, str]]:
  """Extract comment blocks from SQL and replace with placeholders.

  A comment block is defined as one or more comment lines (starting with --)
  that are surrounded by empty lines or start/end of file.

  Args:
    sql: Input SQL string

  Returns:
    A tuple containing:
      - Processed SQL with comment blocks replaced by placeholders
      - Dict mapping placeholders to their original comment blocks
  """
  # Split into chunks separated by empty lines
  chunks = []
  current = []
  for line in sql.splitlines():
    if line.strip():
      current.append(line)
    elif current:
      chunks.append(current)
      current = []
  if current:
    chunks.append(current)

  # Process each chunk
  blocks = {}
  result = []
  for i, chunk in enumerate(chunks):
    # A chunk is a comment block if all lines start with --
    if all(line.strip().startswith('--') for line in chunk):
      placeholder = f'-- __COMMENT_BLOCK_{i}__'
      blocks[placeholder] = '\n'.join(chunk) + '\n'
      result.append('')
      result.append(placeholder)
      result.append('')
    else:
      result.append('')
      result.extend(chunk)
      result.append('')

  return '\n'.join(result).strip(), blocks


def restore_comment_blocks(sql: str, blocks: Dict[str, str]) -> str:
  """Restore comment blocks from their placeholders.

  Args:
    sql: SQL string with placeholders
    blocks: Dict mapping placeholders to original comment blocks

  Returns:
    SQL string with comment blocks restored in their original positions
  """
  result = sql
  for placeholder, block in blocks.items():
    result = result.replace(placeholder, block)
  return result


def format_sql(file: Path,
               sql: str,
               indent_width: int = 2,
               verbose: bool = False) -> str:
  """Format SQL content with consistent style.

  Args:
    file: Path to the SQL file (for error reporting)
    sql: SQL content to format
    indent_width: Number of spaces for indentation
    verbose: Whether to print status messages

  Returns:
    Formatted SQL string

  Raises:
    Exception: If SQL parsing or formatting fails
  """
  if sql.find('-- sqlformat file off') != -1:
    if verbose:
      print(f"Ignoring {file}", file=sys.stderr)
    return sql

  # First extract comment blocks
  sql_with_placeholders, comment_blocks = extract_comment_blocks(sql)

  # Then process macros
  processed, macros = preprocess_macros(sql_with_placeholders)
  try:
    formatted = ''
    for ast in sqlglot.parse(sql=processed, dialect=Perfetto):
      formatted += ast.sql(
          pretty=True,
          dialect=Perfetto,
          indent=indent_width,
          normalize=True,
          normalize_functions='lower',
      )
      formatted += ";\n\n"

    # Restore macros first, then comment blocks
    with_macros = postprocess_macros(formatted, macros)
    return restore_comment_blocks(with_macros, comment_blocks).rstrip() + '\n'
  except Exception as e:
    print(f"Failed to format SQL: file {file}, {e}", file=sys.stderr)
    raise e


def format_files_in_place(paths: List[Union[str, Path]],
                          indent_width: int = 2,
                          verbose: bool = False) -> None:
  """Format multiple SQL files in place.

  Args:
      paths: List of file or directory paths to format
      indent_width: Number of spaces for indentation
      verbose: Whether to print status messages
  """
  for path in paths:
    path = Path(path)
    if path.is_dir():
      # Process all .sql files in directory recursively
      sql_files = path.rglob('*.sql')
    else:
      # Single file
      sql_files = [path]

    for sql_file in sql_files:
      with open(sql_file) as f:
        sql = f.read()
      formatted = format_sql(sql_file, sql, indent_width, verbose)
      with open(sql_file, 'w') as f:
        f.write(formatted)
      if verbose:
        print(f"Formatted {sql_file}", file=sys.stderr)


def check_sql_formatting(paths: List[Union[str, Path]],
                         indent_width: int = 2,
                         verbose: bool = False) -> bool:
  """Check SQL files for formatting violations without making changes.

  Args:
      paths: List of file or directory paths to check
      indent_width: Number of spaces for indentation
      verbose: Whether to print status messages

  Returns:
      True if all files are properly formatted, False otherwise
  """
  all_formatted = True
  for path in paths:
    path = Path(path)
    if path.is_dir():
      sql_files = path.rglob('*.sql')
    else:
      sql_files = [path]

    for sql_file in sql_files:
      with open(sql_file) as f:
        sql = f.read()
      formatted = format_sql(sql_file, sql, indent_width, verbose)
      if formatted != sql:
        print(f"Would format {sql_file}", file=sys.stderr)
        all_formatted = False

  return all_formatted


def main() -> None:
  """Main entry point."""
  parser = argparse.ArgumentParser(
      description='Format SQL queries with consistent style')
  parser.add_argument(
      'paths',
      nargs='*',
      help='Paths to SQL files or directories containing SQL files')
  parser.add_argument(
      '--indent-width',
      type=int,
      default=2,
      help='Number of spaces for indentation (default: 2)')
  parser.add_argument(
      '--in-place', action='store_true', help='Format files in place')
  parser.add_argument(
      '--check-only',
      action='store_true',
      help='Check for formatting violations without making changes')
  parser.add_argument(
      '--verbose',
      action='store_true',
      help='Print status messages during execution')
  args = parser.parse_args()

  if args.check_only:
    properly_formatted = check_sql_formatting(args.paths, args.indent_width,
                                              args.verbose)
    sys.exit(0 if properly_formatted else 1)
  elif args.in_place:
    format_files_in_place(args.paths, args.indent_width, args.verbose)
  else:
    # Read from stdin if no files provided
    if not sys.stdin.isatty():
      sql_input = sys.stdin.read()
      formatted_sql = format_sql(Path("stdin"), sql_input, args.indent_width)
      print(formatted_sql)
    else:
      # Print help if no input provided
      parser.print_help()


if __name__ == '__main__':
  main()
