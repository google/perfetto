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

#ifndef SRC_PERFETTO_SQL_ANALYSIS_SOURCE_TREE_H_
#define SRC_PERFETTO_SQL_ANALYSIS_SOURCE_TREE_H_

#include <memory>
#include <string>

#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/program.h"

namespace perfetto::perfetto_sql::analysis {

// Builds a semantic Program from one or more trees of PerfettoSQL modules.
// This is the source-loading frontend: it owns parsing and macro expansion,
// while Program contains the parser-independent analysis result.
class SourceTreeAnalyzer {
 public:
  SourceTreeAnalyzer();
  ~SourceTreeAnalyzer();

  SourceTreeAnalyzer(const SourceTreeAnalyzer&) = delete;
  SourceTreeAnalyzer& operator=(const SourceTreeAnalyzer&) = delete;

  // All trees share one module namespace. The first tree wins when the same
  // module path occurs in more than one tree.
  void AddTree(std::string root);

  base::StatusOr<Program> Analyze();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace perfetto::perfetto_sql::analysis

#endif  // SRC_PERFETTO_SQL_ANALYSIS_SOURCE_TREE_H_
