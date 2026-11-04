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

#ifndef SRC_TOOLS_PFSQL_SOURCE_TREE_H_
#define SRC_TOOLS_PFSQL_SOURCE_TREE_H_

#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"

namespace perfetto::pfsql {

enum class SymbolKind {
  kTable,
  kView,
  kFunction,
  kMacro,
};

const char* SymbolKindName(SymbolKind);

struct Symbol {
  std::string name;
  SymbolKind kind;
  // Sorted names this symbol's definition refers to, including those defined
  // nowhere in the tree. Macros are not expanded: references in a macro's body
  // belong to the macro and references in its arguments belong to the caller.
  std::vector<std::string> references;
};

struct Module {
  std::string name;
  std::string path;
  std::vector<std::string> includes;
  std::vector<Symbol> symbols;
  // References from statements which define no symbol.
  std::vector<std::string> references;
  std::vector<std::string> errors;
};

// Parses every `.sql` file under `roots`, sorted by module name. All trees
// share one module namespace and the first tree wins when the same module path
// occurs in more than one. This only reports what the SQL says: deciding what
// is allowed is up to the caller.
base::StatusOr<std::vector<Module>> ParseSourceTrees(
    const std::vector<std::string>& roots);

}  // namespace perfetto::pfsql

#endif  // SRC_TOOLS_PFSQL_SOURCE_TREE_H_
