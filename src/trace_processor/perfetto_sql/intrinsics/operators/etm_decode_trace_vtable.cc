/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/operators/etm_decode_trace_vtable.h"

#include <sqlite3.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/importers/etm/element_cursor.h"
#include "src/trace_processor/importers/etm/mapping_version.h"
#include "src/trace_processor/importers/etm/opencsd.h"
#include "src/trace_processor/importers/etm/sql_values.h"
#include "src/trace_processor/importers/etm/util.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor::etm {
namespace {

base::StatusOr<ocsd_gen_trc_elem_t> ToElementType(sqlite3_value* value) {
  SqlValue element_type = sqlite::utils::SqliteValueToSqlValue(value);
  if (element_type.type != SqlValue::kString) {
    return base::ErrStatus(
        "Invalid data type for element_type. Expected STRING");
  }
  std::optional<ocsd_gen_trc_elem_t> type = FromString(element_type.AsString());
  if (!type) {
    return base::ErrStatus("Invalid element_type value: %s",
                           element_type.AsString());
  }
  return *type;
}

base::StatusOr<tables::EtmV4ChunkTable::Id> GetEtmV4ChunkId(
    const TraceStorage* storage,
    sqlite3_value* argv) {
  SqlValue in_id = sqlite::utils::SqliteValueToSqlValue(argv);
  if (in_id.type != SqlValue::kLong) {
    return base::ErrStatus("chunk_id must be LONG");
  }

  if (in_id.AsLong() < 0 ||
      in_id.AsLong() >= storage->etm_v4_chunk_table().row_count()) {
    return base::ErrStatus("Invalid chunk_id value: %" PRIu32,
                           storage->etm_v4_chunk_table().row_count());
  }

  return tables::EtmV4ChunkTable::Id(static_cast<uint32_t>(in_id.AsLong()));
}

static constexpr char kSchema[] = R"(
    CREATE TABLE x(
      chunk_id INTEGER HIDDEN,
      chunk_index INTEGER,
      element_index INTEGER,
      element_type TEXT,
      timestamp INTEGER,
      cycle_count INTEGER,
      last_seen_timestamp INTEGER,
      cumulative_cycles INTEGER,
      exception_level INTEGER,
      context_id INTEGER,
      isa TEXT,
      start_address INTEGER,
      end_address INTEGER,
      mapping_id INTEGER,
      instruction_range BLOB HIDDEN
    )
  )";

enum class ColumnIndex {
  kChunkId,
  kChunkIndex,
  kElementIndex,
  kElementType,
  kTimestamp,
  kCycleCount,
  kLastSeenTimestamp,
  kCumulativeCycles,
  kExceptionLevel,
  kContextId,
  kIsa,
  kStartAddress,
  kEndAddress,
  kMappingId,
  kInstructionRange
};

constexpr char kChunkIdEqArg = 't';
constexpr char kElementTypeEqArg = 'e';
constexpr char kElementTypeInArg = 'E';

}  // namespace

class EtmDecodeChunkVtable::Cursor
    : public sqlite::Module<EtmDecodeChunkVtable>::Cursor {
 public:
  explicit Cursor(Vtab* vtab) : cursor_(vtab->storage) {}

  base::Status Filter(int idxNum,
                      const char* idxStr,
                      int argc,
                      sqlite3_value** argv);
  base::Status Next() {
    if (flushing_buffer_) {
      buffer_idx_++;
      if (buffer_idx_ >= rows_waiting_for_timestamp_.size()) {
        flushing_buffer_ = false;
        rows_waiting_for_timestamp_.clear();
        buffer_idx_ = 0;
      } else {
        return base::OkStatus();
      }
    }

    while (true) {
      RETURN_IF_ERROR(cursor_.Next());
      if (cursor_.Eof()) {
        if (waiting_for_timestamp_ && !rows_waiting_for_timestamp_.empty()) {
          flushing_buffer_ = true;
          buffer_idx_ = 0;
          return base::OkStatus();
        }
        return base::OkStatus();
      }

      if (waiting_for_timestamp_) {
        if (cursor_.element().getType() == OCSD_GEN_TRC_ELEM_TIMESTAMP) {
          last_seen_timestamp_ =
              static_cast<int64_t>(cursor_.element().timestamp);
          waiting_for_timestamp_ = false;

          for (auto& row : rows_waiting_for_timestamp_) {
            if (row.getType() == OCSD_GEN_TRC_ELEM_SYNC_MARKER) {
              row.timestamp = cursor_.element().timestamp;
              row.has_ts = true;
              if (cursor_.element().has_cc) {
                row.cycle_count = cursor_.element().cycle_count;
                row.has_cc = true;
              }
            }
          }
          rows_waiting_for_timestamp_.push_back(cursor_.element());
          flushing_buffer_ = true;
          buffer_idx_ = 0;
          return base::OkStatus();
        }
        rows_waiting_for_timestamp_.push_back(cursor_.element());
        if (rows_waiting_for_timestamp_.size() >= 30) {
          flushing_buffer_ = true;
          buffer_idx_ = 0;
          waiting_for_timestamp_ = false;
        }
      } else {
        if (cursor_.element().getType() == OCSD_GEN_TRC_ELEM_SYNC_MARKER) {
          waiting_for_timestamp_ = true;
          rows_waiting_for_timestamp_.push_back(cursor_.element());
          continue;
        }
        break;
      }
    }
    return base::OkStatus();
  }
  bool Eof() {
    if (flushing_buffer_) {
      return buffer_idx_ >= rows_waiting_for_timestamp_.size();
    }
    return cursor_.Eof();
  }
  int Column(sqlite3_context* ctx, int raw_n);

 private:
  base::StatusOr<ElementTypeMask> GetTypeMask(sqlite3_value* argv,
                                              bool is_inlist);

  ElementCursor cursor_;

  // Stores the last seen timestamp.
  int64_t last_seen_timestamp_ = -1;
  // Stores the cumulative cycle count including timestamp packets.
  int64_t cumulative_cycle_count_ = -1;
  // Stores the last cumulative cycle count using only cycle count packets.
  int64_t last_cc_value_ = 0;
  // Indicates if we are waiting for a timestamp.
  bool waiting_for_timestamp_ = false;
  // Buffer of rows waiting for a timestamp packet (i.e. saw a sync and looking
  // for timestamp)
  std::vector<OcsdTraceElement> rows_waiting_for_timestamp_;
  bool flushing_buffer_ = false;
  size_t buffer_idx_ = 0;
};

base::StatusOr<ElementTypeMask> EtmDecodeChunkVtable::Cursor::GetTypeMask(
    sqlite3_value* argv,
    bool is_inlist) {
  ElementTypeMask mask;
  if (!is_inlist) {
    ASSIGN_OR_RETURN(ocsd_gen_trc_elem_t type, ToElementType(argv));
    mask.set_bit(type);
    return mask;
  }
  int rc;
  sqlite3_value* type_value;
  for (rc = sqlite3_vtab_in_first(argv, &type_value); rc == SQLITE_OK;
       rc = sqlite3_vtab_in_next(argv, &type_value)) {
    ASSIGN_OR_RETURN(ocsd_gen_trc_elem_t type, ToElementType(argv));
    mask.set_bit(type);
  }
  if (rc != SQLITE_OK || rc != SQLITE_DONE) {
    return base::ErrStatus("Error");
  }
  return mask;
}

base::Status EtmDecodeChunkVtable::Cursor::Filter(int,
                                                  const char* idxStr,
                                                  int argc,
                                                  sqlite3_value** argv) {
  last_seen_timestamp_ = -1;
  cumulative_cycle_count_ = -1;
  last_cc_value_ = 0;
  waiting_for_timestamp_ = false;
  rows_waiting_for_timestamp_.clear();
  std::optional<tables::EtmV4ChunkTable::Id> id;
  ElementTypeMask type_mask;
  type_mask.set_all();
  if (argc != static_cast<int>(strlen(idxStr))) {
    return base::ErrStatus("Invalid idxStr");
  }
  for (; *idxStr != 0; ++idxStr, ++argv) {
    switch (*idxStr) {
      case kChunkIdEqArg: {
        ASSIGN_OR_RETURN(id, GetEtmV4ChunkId(cursor_.storage(), *argv));
        break;
      }
      case kElementTypeEqArg: {
        ASSIGN_OR_RETURN(ElementTypeMask tmp, GetTypeMask(*argv, false));
        type_mask &= tmp;
        break;
      }
      case kElementTypeInArg: {
        ASSIGN_OR_RETURN(ElementTypeMask tmp, GetTypeMask(*argv, true));
        type_mask &= tmp;
        break;
      }
      default:
        return base::ErrStatus("Invalid idxStr");
    }
  }

  // Given the BestIndex impl this should not happen!
  PERFETTO_CHECK(id);

  return cursor_.Filter(id, type_mask);
}

int EtmDecodeChunkVtable::Cursor::Column(sqlite3_context* ctx, int raw_n) {
  const OcsdTraceElement* elem = flushing_buffer_
                                     ? &rows_waiting_for_timestamp_[buffer_idx_]
                                     : &cursor_.element();

  switch (static_cast<ColumnIndex>(raw_n)) {
    case ColumnIndex::kChunkId:
      sqlite::result::Long(ctx, cursor_.chunk_id().value);
      break;
    case ColumnIndex::kChunkIndex:
      sqlite::result::Long(ctx, static_cast<int64_t>(cursor_.index()));
      break;
    case ColumnIndex::kElementIndex:
      sqlite::result::Long(ctx, cursor_.element_index());
      break;
    case ColumnIndex::kElementType:
      sqlite::result::StaticString(ctx, ToString(elem->getType()));
      break;
    case ColumnIndex::kTimestamp:
      if (elem->getType() == OCSD_GEN_TRC_ELEM_TIMESTAMP || elem->has_ts) {
        sqlite::result::Long(ctx, static_cast<int64_t>(elem->timestamp));
      }
      break;
    case ColumnIndex::kCycleCount:
      if (elem->has_cc) {
        sqlite::result::Long(ctx, elem->cycle_count);
      }
      break;
    case ColumnIndex::kLastSeenTimestamp:
      if (last_seen_timestamp_ != -1) {
        sqlite::result::Long(ctx, last_seen_timestamp_);
      }
      break;
    case ColumnIndex::kCumulativeCycles:
      if (elem->has_cc) {
        if (elem->getType() == OCSD_GEN_TRC_ELEM_TIMESTAMP ||
            elem->getType() == OCSD_GEN_TRC_ELEM_SYNC_MARKER) {
          cumulative_cycle_count_ = elem->cycle_count + last_cc_value_;
        } else if (elem->getType() == OCSD_GEN_TRC_ELEM_CYCLE_COUNT) {
          last_cc_value_ += elem->cycle_count;
          cumulative_cycle_count_ = last_cc_value_;
        }
      }
      if (cumulative_cycle_count_ != -1) {
        sqlite::result::Long(ctx, cumulative_cycle_count_);
      }
      break;
    case ColumnIndex::kExceptionLevel:
      if (elem->context.el_valid) {
        sqlite::result::Long(ctx, elem->context.exception_level);
      }
      break;
    case ColumnIndex::kContextId:
      if (elem->context.ctxt_id_valid) {
        sqlite::result::Long(ctx, elem->context.context_id);
      }
      break;
    case ColumnIndex::kIsa:
      sqlite::result::StaticString(ctx, ToString(elem->isa));
      break;
    case ColumnIndex::kStartAddress:
      sqlite::result::Long(ctx, static_cast<int64_t>(elem->st_addr));
      break;
    case ColumnIndex::kEndAddress:
      sqlite::result::Long(ctx, static_cast<int64_t>(elem->en_addr));
      break;
    case ColumnIndex::kMappingId:
      if (cursor_.mapping()) {
        sqlite::result::Long(ctx, cursor_.mapping()->id().value);
      }
      break;
    case ColumnIndex::kInstructionRange:
      if (cursor_.has_instruction_range()) {
        sqlite::result::UniquePointer(ctx, cursor_.GetInstructionRange(),
                                      InstructionRangeSqlValue::kPtrType);
      }
      break;
  }

  return SQLITE_OK;
}

int EtmDecodeChunkVtable::Connect(sqlite3* db,
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

int EtmDecodeChunkVtable::Disconnect(sqlite3_vtab* vtab) {
  delete GetVtab(vtab);
  return SQLITE_OK;
}

int EtmDecodeChunkVtable::BestIndex(sqlite3_vtab* tab,
                                    sqlite3_index_info* info) {
  bool seen_id_eq = false;
  int argv_index = 1;
  std::string idx_str;
  for (int i = 0; i < info->nConstraint; ++i) {
    auto& in = info->aConstraint[i];
    auto& out = info->aConstraintUsage[i];

    if (in.iColumn == static_cast<int>(ColumnIndex::kChunkId)) {
      if (!in.usable) {
        return SQLITE_CONSTRAINT;
      }
      if (in.op != SQLITE_INDEX_CONSTRAINT_EQ) {
        return sqlite::utils::SetError(
            tab, "chunk_id only supports equality constraints");
      }
      seen_id_eq = true;

      idx_str += kChunkIdEqArg;
      out.argvIndex = argv_index++;
      out.omit = true;
      continue;
    }
    if (in.usable &&
        in.iColumn == static_cast<int>(ColumnIndex::kElementType)) {
      if (in.op != SQLITE_INDEX_CONSTRAINT_EQ) {
        continue;
      }

      if (sqlite3_vtab_in(info, i, 1)) {
        idx_str += kElementTypeInArg;
      } else {
        idx_str += kElementTypeEqArg;
      }

      out.argvIndex = argv_index++;
      out.omit = true;
      continue;
    }
  }
  if (!seen_id_eq) {
    return sqlite::utils::SetError(tab, "Constraint required on chunk_id");
  }

  info->idxStr = sqlite3_mprintf("%s", idx_str.c_str());
  info->needToFreeIdxStr = true;

  return SQLITE_OK;
}

int EtmDecodeChunkVtable::Open(sqlite3_vtab* sql_vtab,
                               sqlite3_vtab_cursor** cursor) {
  *cursor = new Cursor(GetVtab(sql_vtab));
  return SQLITE_OK;
}

int EtmDecodeChunkVtable::Close(sqlite3_vtab_cursor* cursor) {
  delete GetCursor(cursor);
  return SQLITE_OK;
}

int EtmDecodeChunkVtable::Filter(sqlite3_vtab_cursor* cur,
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

int EtmDecodeChunkVtable::Next(sqlite3_vtab_cursor* cur) {
  auto status = GetCursor(cur)->Next();
  if (!status.ok()) {
    return sqlite::utils::SetError(cur->pVtab, status);
  }
  return SQLITE_OK;
}

int EtmDecodeChunkVtable::Eof(sqlite3_vtab_cursor* cur) {
  return GetCursor(cur)->Eof();
}

int EtmDecodeChunkVtable::Column(sqlite3_vtab_cursor* cur,
                                 sqlite3_context* ctx,
                                 int raw_n) {
  return GetCursor(cur)->Column(ctx, raw_n);
}

int EtmDecodeChunkVtable::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

}  // namespace perfetto::trace_processor::etm
