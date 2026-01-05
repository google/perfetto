/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PREPROCESSOR_PREPROCESSOR_GRAMMAR_INTERFACE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PREPROCESSOR_PREPROCESSOR_GRAMMAR_INTERFACE_H_

#include <stddef.h>
#include <stdio.h>

#include <vector>

#include "src/trace_processor/perfetto_sql/preprocessor/preprocessor_grammar.h"

#undef NDEBUG

namespace perfetto::trace_processor {

struct PreprocessorGrammarToken {
  const char* ptr;
  size_t n;
  int major;
};

struct PreprocessorGrammarTokenBounds {
  PreprocessorGrammarToken start;
  PreprocessorGrammarToken end;
};

// Forward declarations - full definitions in perfetto_sql_preprocessor.cc
struct PreprocessorGrammarState;

struct PreprocessorGrammarApplyList {
  std::vector<PreprocessorGrammarTokenBounds> args;
};

void* PreprocessorGrammarParseAlloc(void* (*)(size_t),
                                    PreprocessorGrammarState*);
void PreprocessorGrammarParse(void* parser, int, PreprocessorGrammarToken);
void PreprocessorGrammarParseFree(void* parser, void (*)(void*));
void PreprocessorGrammarParseTrace(FILE*, char*);

void OnPreprocessorSyntaxError(PreprocessorGrammarState*,
                               PreprocessorGrammarToken*);
void OnPreprocessorApply(PreprocessorGrammarState*,
                         PreprocessorGrammarToken* name,
                         PreprocessorGrammarToken* join,
                         PreprocessorGrammarToken* prefix,
                         PreprocessorGrammarApplyList*,
                         PreprocessorGrammarApplyList*);
void OnPreprocessorVariable(PreprocessorGrammarState*,
                            PreprocessorGrammarToken* var);
void OnPreprocessorMacroId(PreprocessorGrammarState*,
                           PreprocessorGrammarToken* name);
void OnPreprocessorMacroArg(PreprocessorGrammarState*,
                            PreprocessorGrammarTokenBounds*);
void OnPreprocessorMacroEnd(PreprocessorGrammarState*,
                            PreprocessorGrammarToken* name,
                            PreprocessorGrammarToken* rp);
void OnPreprocessorEnd(PreprocessorGrammarState*);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PREPROCESSOR_PREPROCESSOR_GRAMMAR_INTERFACE_H_
