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

#ifndef SRC_PERFETTO_SQL_ANALYSIS_PROGRAM_H_
#define SRC_PERFETTO_SQL_ANALYSIS_PROGRAM_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace perfetto::perfetto_sql::analysis {

struct ModuleId {
  uint32_t value;

  bool operator==(ModuleId other) const { return value == other.value; }
  bool operator!=(ModuleId other) const { return value != other.value; }
};

struct SymbolId {
  uint32_t value;

  bool operator==(SymbolId other) const { return value == other.value; }
  bool operator!=(SymbolId other) const { return value != other.value; }
};

enum class SymbolKind {
  kTable,
  kView,
  kFunction,
  kMacro,
};

enum class ReferenceKind {
  kRelation,
  kFunction,
  kMacro,
};

const char* SymbolKindName(SymbolKind);

struct SymbolReference {
  SymbolId symbol_id;
  ReferenceKind kind;
};

struct UnresolvedReference {
  std::string_view name;
  ReferenceKind kind;
};

struct Symbol {
  std::string_view name;
  SymbolKind kind;
  ModuleId module_id;
  std::vector<SymbolReference> references;
  std::vector<UnresolvedReference> unresolved_references;
};

struct Module {
  std::string_view name;
  std::string_view path;
  std::vector<SymbolId> symbol_ids;
  std::vector<std::string_view> declared_includes;
  std::vector<std::string_view> diagnostics;
};

// An immutable semantic graph of a PerfettoSQL program. All strings and graph
// nodes are owned by this object.
class Program {
 public:
  Program(Program&&) noexcept;
  Program& operator=(Program&&) noexcept;
  ~Program();

  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  const std::vector<Module>& modules() const;
  const std::vector<Symbol>& symbols() const;
  const Module& module(ModuleId) const;
  const Symbol& symbol(SymbolId) const;

  std::optional<ModuleId> FindModule(std::string_view name) const;
  std::optional<SymbolId> FindSymbol(std::string_view name) const;

 private:
  class Storage;

  explicit Program(std::unique_ptr<Storage>);

  std::unique_ptr<Storage> storage_;

  friend class ProgramBuilder;
};

// Mutable construction API for Program. References are supplied by name and
// resolved into graph edges by Build().
class ProgramBuilder {
 public:
  ProgramBuilder();
  ~ProgramBuilder();

  ProgramBuilder(const ProgramBuilder&) = delete;
  ProgramBuilder& operator=(const ProgramBuilder&) = delete;

  ModuleId AddModule(std::string name, std::string path);
  void AddDeclaredInclude(ModuleId, std::string module_name);
  void AddDiagnostic(ModuleId, std::string message);

  SymbolId AddSymbol(ModuleId, std::string name, SymbolKind);
  void AddReference(SymbolId, std::string referenced_name, ReferenceKind);

  Program Build();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace perfetto::perfetto_sql::analysis

#endif  // SRC_PERFETTO_SQL_ANALYSIS_PROGRAM_H_
