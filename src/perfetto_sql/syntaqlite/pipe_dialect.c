/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The dialect template the formatter library exports: the generated one
// with its tokenizer replaced by the one which knows `|>`. The generated
// template is compiled into this library under the name
// syntaqlite_perfetto_generated_dialect_template (see BUILD.gn), which this
// file undoes for itself so it can define the exported name.

#include <stdint.h>

#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

#undef syntaqlite_perfetto_dialect_template

#include "src/perfetto_sql/syntaqlite/pipe_token.h"

SYNTAQLITE_API const SyntaqliteDialectTemplate*
syntaqlite_perfetto_generated_dialect_template(void);

SYNTAQLITE_API const SyntaqliteDialectTemplate*
syntaqlite_perfetto_dialect_template(void) {
  static SyntaqliteDialectTemplate with_pipe;
  static int filled = 0;
  if (!filled) {
    with_pipe = *syntaqlite_perfetto_generated_dialect_template();
    with_pipe.get_token = SynqPerfettoGetTokenWithPipe;
    filled = 1;
  }
  return &with_pipe;
}
