/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/operators/symbolize_vtable.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/profiling/symbolizer/local_symbolizer.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::etm {
namespace {

static constexpr char kSchema[] = R"(
    CREATE TABLE x(
      rel_pc INTEGER,
      inline_depth INTEGER,
      is_most_inlined INTEGER,
      function_name TEXT,
      file_name TEXT,
      line INTEGER,
      mapping_id INTEGER HIDDEN,
      address INTEGER HIDDEN
    )
  )";

enum class ColumnIndex {
  kRelPc,
  kInlineDepth,
  kIsMostInlined,
  kFunctionName,
  kFileName,
  kLine,
  kMappingId,
  kAddress,
};

constexpr char kAddressEqArg = 'a';
constexpr char kMapingIdEqArg = 'm';

}  // namespace

class SymbolizeVtable::Cursor : public sqlite::Module<SymbolizeVtable>::Cursor {
 public:
  explicit Cursor(Vtab* vtab) : vtab_(vtab) {}
  base::Status Filter(int, const char*, int, sqlite3_value**);
  int Next();
  int Eof();
  int Column(sqlite3_context*, int);

 private:
  struct Filters {
    MappingId mapping_id;
    uint64_t address;
  };

  void SymbolizeMapping(MappingId mapping_id, uint64_t address);

  base::StatusOr<std::optional<Filters>> GetFilters(const char* idx_str,
                                                    int argc,
                                                    sqlite3_value** argv);

  base::StatusOr<std::optional<MappingId>> GetMappingId(
      sqlite3_value* argv) const;
  base::StatusOr<std::optional<uint64_t>> GetAddress(sqlite3_value* argv) const;

  Vtab* vtab_;
  Filters filters_;
  uint64_t rel_pc_;
  std::vector<profiling::SymbolizedFrame> frames_;
};

SymbolizeVtable::Vtab::Vtab(TraceStorage* storage) : storage_(storage) {
  llvm_ = std::make_unique<profiling::LLVMSymbolizerProcess>("llvm-symbolizer");
}

void SymbolizeVtable::Cursor::SymbolizeMapping(MappingId mapping_id,
                                               uint64_t address) {
  auto storage = vtab_->storage();
  auto mapping = storage->stack_profile_mapping_table().FindById(mapping_id);

  const auto& elf_table = storage->elf_file_table();

  Query q;
  q.constraints = {
      elf_table.build_id().eq(storage->GetString(mapping->build_id()))};
  auto elf = elf_table.FilterToIterator(q);
  if (!elf) {
    return;
  }

  std::string file_name =
      storage->GetString(storage->file_table().FindById(elf.file_id())->name())
          .ToStdString();

  rel_pc_ = address - static_cast<uint64_t>(mapping->start()) +
            static_cast<uint64_t>(mapping->exact_offset()) +
            static_cast<uint64_t>(elf.load_bias());

  frames_ = vtab_->llvm()->Symbolize(file_name, rel_pc_);
}

base::StatusOr<std::optional<uint64_t>> SymbolizeVtable::Cursor::GetAddress(
    sqlite3_value* argv) const {
  SqlValue in_id = sqlite::utils::SqliteValueToSqlValue(argv);
  if (in_id.is_null()) {
    return {std::nullopt};
  }

  if (in_id.type != SqlValue::kLong) {
    return {std::nullopt};
  }

  return std::make_optional(static_cast<uint64_t>(in_id.AsLong()));
}

base::StatusOr<std::optional<MappingId>> SymbolizeVtable::Cursor::GetMappingId(
    sqlite3_value* argv) const {
  SqlValue in_id = sqlite::utils::SqliteValueToSqlValue(argv);
  if (in_id.is_null()) {
    return {std::nullopt};
  }

  if (in_id.type != SqlValue::kLong) {
    return {std::nullopt};
  }

  if (in_id.AsLong() < 0 ||
      in_id.AsLong() >=
          vtab_->storage()->stack_profile_mapping_table().row_count()) {
    return {std::nullopt};
  }

  return std::make_optional(MappingId(static_cast<uint32_t>(in_id.AsLong())));
}

base::StatusOr<std::optional<SymbolizeVtable::Cursor::Filters>>
SymbolizeVtable::Cursor::GetFilters(const char* idx_str,
                                    int argc,
                                    sqlite3_value** argv) {
  if (argc != static_cast<int>(strlen(idx_str))) {
    return base::ErrStatus("Invalid idxStr");
  }
  Filters filters;
  bool m_set = false;
  bool a_set = false;
  for (; *idx_str != 0; ++idx_str, ++argv) {
    switch (*idx_str) {
      case kMapingIdEqArg: {
        ASSIGN_OR_RETURN(auto m, GetMappingId(*argv));
        if (!m || (m_set && filters.mapping_id != *m)) {
          return {std::nullopt};
        }
        filters.mapping_id = *m;
        break;
      }
      case kAddressEqArg: {
        ASSIGN_OR_RETURN(auto a, GetAddress(*argv));
        if (!a || (a_set && filters.address != *a)) {
          return {std::nullopt};
        }
        filters.address = *a;
        break;
      }
      default:
        return base::ErrStatus("Invalid idxStr");
    }
  }

  return {filters};
}

base::Status SymbolizeVtable::Cursor::Filter(int,
                                             const char* idx_str,
                                             int argc,
                                             sqlite3_value** argv) {
  frames_.clear();
  ASSIGN_OR_RETURN(auto filters, GetFilters(idx_str, argc, argv));

  if (!filters) {
    return base::OkStatus();
  }

  filters_ = *filters;
  SymbolizeMapping(filters->mapping_id, filters->address);
  return base::OkStatus();
}

int SymbolizeVtable::Cursor::Next() {
  frames_.pop_back();
  return SQLITE_OK;
}

int SymbolizeVtable::Cursor::Eof() {
  return frames_.empty();
}

int SymbolizeVtable::Cursor::Column(sqlite3_context* ctx, int raw_n) {
  switch (static_cast<ColumnIndex>(raw_n)) {
    case ColumnIndex::kRelPc:
      sqlite::result::Long(ctx, static_cast<int64_t>(rel_pc_));
      break;
    case ColumnIndex::kAddress:
      sqlite::result::Long(ctx, static_cast<int64_t>(filters_.address));
      break;

    case ColumnIndex::kMappingId:
      sqlite::result::Long(ctx, filters_.mapping_id.value);
      break;

    case ColumnIndex::kInlineDepth:
      sqlite::result::Long(ctx, static_cast<int64_t>(frames_.size() - 1));
      break;

    case ColumnIndex::kFunctionName:
      sqlite::result::TransientString(ctx,
                                      frames_.back().function_name.c_str());
      break;

    case ColumnIndex::kFileName:
      sqlite::result::TransientString(ctx, frames_.back().file_name.c_str());
      break;
    case ColumnIndex::kLine:
      sqlite::result::Long(ctx, frames_.back().line);
      break;
    case ColumnIndex::kIsMostInlined:
      sqlite::result::Long(ctx, frames_.size() == 1);
      break;
  }
  return SQLITE_OK;
}

int SymbolizeVtable::Connect(sqlite3* db,
                             void* ctx,
                             int,
                             const char* const*,
                             sqlite3_vtab** vtab,
                             char**) {
  if (int ret = sqlite3_declare_vtab(db, kSchema); ret != SQLITE_OK) {
    return ret;
  }
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>(GetContext(ctx));
  *vtab = res.release();
  return SQLITE_OK;
}

int SymbolizeVtable::Disconnect(sqlite3_vtab* vtab) {
  delete GetVtab(vtab);
  return SQLITE_OK;
}

int SymbolizeVtable::BestIndex(sqlite3_vtab* tab, sqlite3_index_info* info) {
  bool seen_mapping_id = false;
  bool seen_address = false;
  int argv_index = 1;
  std::string idx_str;
  for (int i = 0; i < info->nConstraint; ++i) {
    auto& in = info->aConstraint[i];
    auto& out = info->aConstraintUsage[i];
    if (in.iColumn == static_cast<int>(ColumnIndex::kMappingId)) {
      if (!in.usable) {
        return SQLITE_CONSTRAINT;
      }
      if (in.op != SQLITE_INDEX_CONSTRAINT_EQ) {
        return sqlite::utils::SetError(
            tab, "mapping_id only supports equality constraints");
      }
      idx_str += kMapingIdEqArg;
      out.argvIndex = argv_index++;
      out.omit = true;
      seen_mapping_id = true;
      continue;
    }

    if (in.iColumn == static_cast<int>(ColumnIndex::kAddress)) {
      if (!in.usable) {
        return SQLITE_CONSTRAINT;
      }
      if (in.op != SQLITE_INDEX_CONSTRAINT_EQ) {
        return sqlite::utils::SetError(
            tab, "address only supports equality constraints");
      }
      seen_address = true;
      idx_str += kAddressEqArg;
      out.argvIndex = argv_index++;
      out.omit = true;
      continue;
    }
  }

  if (!seen_mapping_id) {
    return sqlite::utils::SetError(tab, "Constraint required on mapping_id");
  }
  if (!seen_address) {
    return sqlite::utils::SetError(tab, "Constraint required on address");
  }

  info->idxStr = sqlite3_mprintf("%s", idx_str.c_str());
  info->needToFreeIdxStr = true;

  return SQLITE_OK;
}

int SymbolizeVtable::Open(sqlite3_vtab* sql_vtab,
                          sqlite3_vtab_cursor** cursor) {
  *cursor = new Cursor(GetVtab(sql_vtab));
  return SQLITE_OK;
}

int SymbolizeVtable::Close(sqlite3_vtab_cursor* cursor) {
  delete GetCursor(cursor);
  return SQLITE_OK;
}

int SymbolizeVtable::Filter(sqlite3_vtab_cursor* cur,
                            int idxNum,
                            const char* idxStr,
                            int argc,
                            sqlite3_value** argv) {
  auto status = GetCursor(cur)->Filter(idxNum, idxStr, argc, argv);
  if (!status.ok()) {
    return sqlite::utils::SetError(cur->pVtab, status);
  }
  return SQLITE_OK;
}

int SymbolizeVtable::Next(sqlite3_vtab_cursor* cur) {
  return GetCursor(cur)->Next();
}

int SymbolizeVtable::Eof(sqlite3_vtab_cursor* cur) {
  return GetCursor(cur)->Eof();
}

int SymbolizeVtable::Column(sqlite3_vtab_cursor* cur,
                            sqlite3_context* ctx,
                            int raw_n) {
  return GetCursor(cur)->Column(ctx, raw_n);
}

int SymbolizeVtable::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

}  // namespace perfetto::trace_processor::etm
