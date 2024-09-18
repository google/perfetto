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

import argparse
import os
import subprocess
import shutil
import sys
import tempfile

GRAMMAR_FOOTER = '''
%token SPACE ILLEGAL.
'''

KEYWORDHASH_HEADER = '''
#include "src/trace_processor/perfetto_sql/grammar/perfettosql_keywordhash_helper.h"
'''

KEYWORD_END = '''
  { "WITHOUT",          "TK_WITHOUT",      ALWAYS,           1      },
};'''

KEYWORD_END_REPLACE = '''
  { "WITHOUT",          "TK_WITHOUT",      ALWAYS,           1      },
  { "PERFETTO",         "TK_PERFETTO",     ALWAYS,           1      },
  { "MACRO",            "TK_MACRO",        ALWAYS,           1      },
  { "INCLUDE",          "TK_INCLUDE",      ALWAYS,           1      },
  { "MODULE",           "TK_MODULE",       ALWAYS,           1      },
  { "RETURNS",          "TK_RETURNS",      ALWAYS,           1      },
  { "FUNCTION",         "TK_FUNCTION",     ALWAYS,           1      },
};'''


def copy_tokenizer(args: argparse.Namespace):
  shutil.copy(args.sqlite_tokenize, args.sqlite_tokenize_out)

  with open(args.sqlite_tokenize_out, 'r+', encoding='utf-8') as fp:
    res: str = fp.read()
    idx = res.find('/*\n** Run the parser on the given SQL string.')
    assert idx != -1
    res = res[0:idx]
    res = res.replace(
        '#include "sqliteInt.h"',
        '#include "src/trace_processor/perfetto_sql/tokenizer/tokenize_internal_helper.h"',
    )
    res = res.replace('#include "keywordhash.h"\n', '')
    fp.seek(0)
    fp.write(res)
    fp.truncate()


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '--lemon', default=os.path.normpath('buildtools/sqlite_src/tool/lemon.c'))
  parser.add_argument(
      '--mkkeywordhash',
      default=os.path.normpath('buildtools/sqlite_src/tool/mkkeywordhash.c'))
  parser.add_argument(
      '--lemon-template',
      default=os.path.normpath('buildtools/sqlite_src/tool/lempar.c'))
  parser.add_argument(
      '--clang', default=os.path.normpath('buildtools/linux64/clang/bin/clang'))
  parser.add_argument(
      '--preprocessor-grammar',
      default=os.path.normpath(
          'src/trace_processor/perfetto_sql/preprocessor/preprocessor_grammar.y'
      ),
  )
  parser.add_argument(
      '--sqlite-grammar',
      default=os.path.normpath('buildtools/sqlite_src/src/parse.y'),
  )
  parser.add_argument(
      '--perfettosql-grammar-include',
      default=os.path.normpath(
          'src/trace_processor/perfetto_sql/grammar/perfettosql_include.y'),
  )
  parser.add_argument(
      '--grammar-out',
      default=os.path.join(
          os.path.normpath('src/trace_processor/perfetto_sql/grammar/')),
  )
  parser.add_argument(
      '--sqlite-tokenize',
      default=os.path.normpath('buildtools/sqlite_src/src/tokenize.c'),
  )
  parser.add_argument(
      '--sqlite-tokenize-out',
      default=os.path.join(
          os.path.normpath(
              'src/trace_processor/perfetto_sql/tokenizer/tokenize_internal.c')
      ),
  )
  args = parser.parse_args()

  with tempfile.TemporaryDirectory() as tmp:
    # Preprocessor grammar
    subprocess.check_call([
        args.clang,
        os.path.join(args.lemon), '-o',
        os.path.join(tmp, 'lemon')
    ])
    shutil.copy(args.lemon_template, tmp)
    subprocess.check_call([
        os.path.join(tmp, 'lemon'),
        args.preprocessor_grammar,
        '-q',
        '-l',
        '-s',
    ])

    # PerfettoSQL keywords
    keywordhash_tmp = os.path.join(tmp, 'mkkeywordhash.c')
    shutil.copy(args.mkkeywordhash, keywordhash_tmp)

    with open(keywordhash_tmp, "r+") as fp:
      keyword_source = fp.read()
      assert keyword_source.find(KEYWORD_END) != -1
      fp.seek(0)
      fp.write(keyword_source.replace(KEYWORD_END, KEYWORD_END_REPLACE))
      fp.truncate()

    subprocess.check_call([
        args.clang,
        os.path.join(keywordhash_tmp), '-o',
        os.path.join(tmp, 'mkkeywordhash')
    ])
    keywordhash_res = subprocess.check_output(
        [os.path.join(tmp, 'mkkeywordhash')]).decode()

    with open(os.path.join(args.grammar_out, "perfettosql_keywordhash.h"),
              "w") as g:
      idx = keywordhash_res.find('#define SQLITE_N_KEYWORD')
      assert idx != -1
      keywordhash_res = keywordhash_res[0:idx]
      g.write(KEYWORDHASH_HEADER)
      g.write(keywordhash_res)

    # PerfettoSQL grammar
    sqlite_grammar = subprocess.check_output([
        os.path.join(tmp, 'lemon'),
        args.sqlite_grammar,
        '-g',
    ]).decode()
    with open(os.path.join(args.grammar_out, "perfettosql_grammar.y"),
              "w") as g:
      with open(args.perfettosql_grammar_include, 'r') as i:
        g.write(i.read())
      g.write(sqlite_grammar)
      g.write(GRAMMAR_FOOTER)
    subprocess.check_call([
        os.path.join(tmp, 'lemon'),
        os.path.join(args.grammar_out, "perfettosql_grammar.y"),
        '-q',
        '-l',
        '-s',
    ])

  copy_tokenizer(args)

  return 0


if __name__ == '__main__':
  sys.exit(main())
