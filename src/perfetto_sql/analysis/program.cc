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

#include "src/perfetto_sql/analysis/program.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/perfetto_sql/analysis/string_arena.h"

namespace perfetto::perfetto_sql::analysis {
namespace {

struct PendingReference {
  std::string name;
  ReferenceKind kind;
};

struct PendingSymbol {
  std::string name;
  SymbolKind kind;
  ModuleId module_id;
  std::vector<PendingReference> references;
};

struct PendingModule {
  std::string name;
  std::string path;
  std::vector<SymbolId> symbol_ids;
  std::vector<std::string> declared_includes;
  std::vector<std::string> diagnostics;
};

size_t StringBytes(const std::vector<PendingModule>& modules,
                   const std::vector<PendingSymbol>& symbols) {
  size_t bytes = 0;
  for (const PendingModule& module : modules) {
    bytes += module.name.size() + module.path.size();
    for (const std::string& include : module.declared_includes) {
      bytes += include.size();
    }
    for (const std::string& diagnostic : module.diagnostics) {
      bytes += diagnostic.size();
    }
  }
  for (const PendingSymbol& symbol : symbols) {
    bytes += symbol.name.size();
    for (const PendingReference& reference : symbol.references) {
      bytes += reference.name.size();
    }
  }
  return bytes;
}

}  // namespace

class Program::Storage {
 public:
  Storage(std::vector<PendingModule> pending_modules,
          std::vector<PendingSymbol> pending_symbols)
      : strings_(StringBytes(pending_modules, pending_symbols)) {
    modules_.reserve(pending_modules.size());
    symbols_.reserve(pending_symbols.size());

    for (const PendingModule& pending : pending_modules) {
      Module module;
      module.name = strings_.Append(pending.name);
      module.path = strings_.Append(pending.path);
      module.symbol_ids = pending.symbol_ids;
      module.declared_includes.reserve(pending.declared_includes.size());
      for (const std::string& include : pending.declared_includes) {
        module.declared_includes.push_back(strings_.Append(include));
      }
      module.diagnostics.reserve(pending.diagnostics.size());
      for (const std::string& diagnostic : pending.diagnostics) {
        module.diagnostics.push_back(strings_.Append(diagnostic));
      }
      module_by_name_.Insert(std::string(module.name),
                             ModuleId{static_cast<uint32_t>(modules_.size())});
      modules_.push_back(std::move(module));
    }

    for (const PendingSymbol& pending : pending_symbols) {
      Symbol symbol;
      symbol.name = strings_.Append(pending.name);
      symbol.kind = pending.kind;
      symbol.module_id = pending.module_id;
      SymbolId id{static_cast<uint32_t>(symbols_.size())};
      if (!symbol_by_name_.Find(std::string(symbol.name))) {
        symbol_by_name_.Insert(std::string(symbol.name), id);
      }
      symbols_.push_back(std::move(symbol));
    }

    for (uint32_t i = 0; i < pending_symbols.size(); ++i) {
      const PendingSymbol& pending = pending_symbols[i];
      Symbol& symbol = symbols_[i];
      for (const PendingReference& reference : pending.references) {
        const SymbolId* target = symbol_by_name_.Find(reference.name);
        if (!target) {
          symbol.unresolved_references.push_back(
              {strings_.Append(reference.name), reference.kind});
          continue;
        }
        if (symbols_[target->value].module_id == symbol.module_id) {
          continue;
        }
        symbol.references.push_back({*target, reference.kind});
      }
    }
  }

  const std::vector<Module>& modules() const { return modules_; }
  const std::vector<Symbol>& symbols() const { return symbols_; }

  std::optional<ModuleId> FindModule(std::string_view name) const {
    const ModuleId* id = module_by_name_.Find(std::string(name));
    return id ? std::make_optional(*id) : std::nullopt;
  }

  std::optional<SymbolId> FindSymbol(std::string_view name) const {
    const SymbolId* id = symbol_by_name_.Find(std::string(name));
    return id ? std::make_optional(*id) : std::nullopt;
  }

 private:
  internal::StringArena strings_;
  std::vector<Module> modules_;
  std::vector<Symbol> symbols_;
  base::FlatHashMap<std::string, ModuleId> module_by_name_;
  base::FlatHashMap<std::string, SymbolId> symbol_by_name_;
};

class ProgramBuilder::Impl {
 public:
  std::vector<PendingModule> modules;
  std::vector<PendingSymbol> symbols;
};

const char* SymbolKindName(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::kTable:
      return "table";
    case SymbolKind::kView:
      return "view";
    case SymbolKind::kFunction:
      return "function";
    case SymbolKind::kMacro:
      return "macro";
  }
  PERFETTO_FATAL("Unknown PerfettoSQL symbol kind");
}

Program::Program(std::unique_ptr<Storage> storage)
    : storage_(std::move(storage)) {}
Program::Program(Program&&) noexcept = default;
Program& Program::operator=(Program&&) noexcept = default;
Program::~Program() = default;

const std::vector<Module>& Program::modules() const {
  return storage_->modules();
}

const std::vector<Symbol>& Program::symbols() const {
  return storage_->symbols();
}

const Module& Program::module(ModuleId id) const {
  return storage_->modules()[id.value];
}

const Symbol& Program::symbol(SymbolId id) const {
  return storage_->symbols()[id.value];
}

std::optional<ModuleId> Program::FindModule(std::string_view name) const {
  return storage_->FindModule(name);
}

std::optional<SymbolId> Program::FindSymbol(std::string_view name) const {
  return storage_->FindSymbol(name);
}

ProgramBuilder::ProgramBuilder() : impl_(std::make_unique<Impl>()) {}
ProgramBuilder::~ProgramBuilder() = default;

ModuleId ProgramBuilder::AddModule(std::string name, std::string path) {
  ModuleId id{static_cast<uint32_t>(impl_->modules.size())};
  impl_->modules.push_back({std::move(name), std::move(path), {}, {}, {}});
  return id;
}

void ProgramBuilder::AddDeclaredInclude(ModuleId id, std::string module_name) {
  impl_->modules[id.value].declared_includes.push_back(std::move(module_name));
}

void ProgramBuilder::AddDiagnostic(ModuleId id, std::string message) {
  impl_->modules[id.value].diagnostics.push_back(std::move(message));
}

SymbolId ProgramBuilder::AddSymbol(ModuleId module_id,
                                   std::string name,
                                   SymbolKind kind) {
  SymbolId id{static_cast<uint32_t>(impl_->symbols.size())};
  impl_->symbols.push_back({std::move(name), kind, module_id, {}});
  impl_->modules[module_id.value].symbol_ids.push_back(id);
  return id;
}

void ProgramBuilder::AddReference(SymbolId symbol_id,
                                  std::string referenced_name,
                                  ReferenceKind kind) {
  impl_->symbols[symbol_id.value].references.push_back(
      {std::move(referenced_name), kind});
}

Program ProgramBuilder::Build() {
  return Program(std::make_unique<Program::Storage>(std::move(impl_->modules),
                                                    std::move(impl_->symbols)));
}

}  // namespace perfetto::perfetto_sql::analysis
