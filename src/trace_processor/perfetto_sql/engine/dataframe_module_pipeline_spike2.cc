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

#include "src/trace_processor/perfetto_sql/engine/dataframe_module_pipeline_spike2.h"

#include <mach/mach_time.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor::pipeline_spike2 {

struct Fetcher : core::ValueFetcher {
  using Type = sqlite::Type;
  static const Type kInt64 = sqlite::Type::kInteger;
  static const Type kDouble = sqlite::Type::kFloat;
  static const Type kString = sqlite::Type::kText;
  static const Type kNull = sqlite::Type::kNull;
  int64_t GetInt64Value(uint32_t i) const {
    return sqlite::value::Int64(argv[i]);
  }
  double GetDoubleValue(uint32_t i) const {
    return sqlite::value::Double(argv[i]);
  }
  const char* GetStringValue(uint32_t i) const {
    return sqlite::value::Text(argv[i]);
  }
  Type GetValueType(uint32_t i) const { return sqlite::value::Type(argv[i]); }
  bool IteratorInit(uint32_t) const { PERFETTO_FATAL("No IN"); }
  bool IteratorNext(uint32_t) const { PERFETTO_FATAL("No IN"); }
  sqlite3_value** argv;
};

namespace {

namespace ex = core::exec;
namespace df = core::dataframe;
namespace lp = core::dataframe::logical;
using core::StorageType;
using core::filter::CastFilterValueResult;

std::atomic<uint64_t> g_pipeline{0};
std::atomic<uint64_t> g_interpreter{0};
std::atomic<uint64_t> g_plans{0};
std::atomic<uint64_t> g_unlowered[16];
const char* const kUnlowered[] = {
    "scan",   "filter_strategy", "filter_op", "index_op",
    "empty",  "distinct",        "reverse",   "sort",
    "minmax", "limit",           "output",    "index_order"};

void PrintCounts() {
  fprintf(stderr,
          "PIPELINE_SPIKE2 filters: pipeline=%" PRIu64 " interpreter=%" PRIu64
          " plans lowered=%" PRIu64 "\n",
          g_pipeline.load(), g_interpreter.load(), g_plans.load());
  for (size_t i = 0; i < sizeof(kUnlowered) / sizeof(kUnlowered[0]); ++i) {
    if (g_unlowered[i]) {
      fprintf(stderr, "PIPELINE_SPIKE2 not lowered (%s): %" PRIu64 "\n",
              kUnlowered[i], g_unlowered[i].load());
    }
  }
}

template <typename T, typename O>
void CastAs(uint32_t index, sqlite3_value** argv, CastFilterValueResult* out) {
  Fetcher fetcher;
  fetcher.argv = argv;
  auto type = fetcher.GetValueType(index);
  if constexpr (std::is_same_v<T, core::String>) {
    const char* value = nullptr;
    out->validity = core::filter::CastFilterValueToString<Fetcher>(
        index, type, fetcher, core::StringOp(O{}), value);
    out->value = value;
  } else if constexpr (std::is_same_v<T, core::Id>) {
    uint32_t value = 0;
    out->validity = core::filter::CastFilterValueToInteger<uint32_t>(
        index, type, fetcher, core::NonStringOp(O{}), value);
    out->value = CastFilterValueResult::Id{value};
  } else {
    T value = 0;
    out->validity = core::filter::CastFilterValueToIntegerOrDouble(
        index, type, fetcher, core::NonStringOp(O{}), value);
    out->value = value;
  }
}

template <typename T>
Caster CasterForOp(core::Op op) {
  switch (op.index()) {
    case core::Op::GetTypeIndex<core::Eq>():
      return &CastAs<T, core::Eq>;
    case core::Op::GetTypeIndex<core::Ne>():
      return &CastAs<T, core::Ne>;
    case core::Op::GetTypeIndex<core::Lt>():
      return &CastAs<T, core::Lt>;
    case core::Op::GetTypeIndex<core::Le>():
      return &CastAs<T, core::Le>;
    case core::Op::GetTypeIndex<core::Gt>():
      return &CastAs<T, core::Gt>;
    case core::Op::GetTypeIndex<core::Ge>():
      return &CastAs<T, core::Ge>;
    default:
      if constexpr (std::is_same_v<T, core::String>) {
        if (op.Is<core::Glob>()) {
          return &CastAs<T, core::Glob>;
        }
        if (op.Is<core::Regex>()) {
          return &CastAs<T, core::Regex>;
        }
      }
      PERFETTO_FATAL("Unsupported op");
  }
}

Caster CasterFor(StorageType storage, core::Op op) {
  switch (storage.index()) {
    case StorageType::GetTypeIndex<core::Id>():
      return CasterForOp<core::Id>(op);
    case StorageType::GetTypeIndex<core::Uint32>():
      return CasterForOp<uint32_t>(op);
    case StorageType::GetTypeIndex<core::Int32>():
      return CasterForOp<int32_t>(op);
    case StorageType::GetTypeIndex<core::Int64>():
      return CasterForOp<int64_t>(op);
    case StorageType::GetTypeIndex<core::Double>():
      return CasterForOp<double>(op);
    default:
      return CasterForOp<core::String>(op);
  }
}

IntegerCast IntegerFor(StorageType storage) {
  switch (storage.index()) {
    case StorageType::GetTypeIndex<core::Id>():
      return IntegerCast::kId;
    case StorageType::GetTypeIndex<core::Uint32>():
      return IntegerCast::kUint32;
    case StorageType::GetTypeIndex<core::Int32>():
      return IntegerCast::kInt32;
    case StorageType::GetTypeIndex<core::Int64>():
      return IntegerCast::kInt64;
    default:
      return IntegerCast::kNone;
  }
}

bool Narrowable(StorageType storage, core::Op op) {
  return !storage.Is<core::String>() &&
         (op.Is<core::Eq>() || op.Is<core::Lt>() || op.Is<core::Le>() ||
          op.Is<core::Gt>() || op.Is<core::Ge>());
}

// Why `plan` cannot be lowered, as an index into kUnlowered, or -1.
int Unlowerable(const df::LogicalPlan& plan) {
  bool filtered = false;
  bool indexed = false;
  for (const lp::Operation& op : plan.ops) {
    switch (op.index()) {
      case 0:  // Scan
        break;
      case 1: {  // Filter
        const auto& f = std::get<lp::Filter>(op);
        if (std::holds_alternative<lp::BinarySearch>(f.strategy)) {
          if (!Narrowable(f.storage, f.op) || filtered || indexed) {
            return 1;
          }
          continue;
        }
        if (std::holds_alternative<lp::SetIdSortedSearch>(f.strategy)) {
          return 1;
        }
        if (!ex::ColumnFilter::Supports(f.storage, f.op)) {
          return 2;
        }
        filtered = true;
        break;
      }
      case 2: {  // IndexFilter
        if (filtered || indexed) {
          return 11;
        }
        for (const auto& p : std::get<lp::IndexFilter>(op).predicates) {
          if (!Narrowable(p.storage, p.op) || !p.op.Is<core::Eq>()) {
            return 3;
          }
        }
        indexed = true;
        break;
      }
      case 3:  // Empty
        break;
      case 4:
        return 5;
      case 5:
        return 6;
      case 6:
        return 7;
      case 7:
        return 8;
      case 8:
        return 9;
      case 9:  // Output
        break;
      default:
        return 0;
    }
  }
  return -1;
}

std::mutex g_mutex;
std::vector<std::unique_ptr<df::LogicalPlan>>& Plans() {
  static auto* plans = new std::vector<std::unique_ptr<df::LogicalPlan>>();
  return *plans;
}

// Reads a cell of a column whose value type, null layout and row shape are
// known when the plan is built: rows arrive as a range when nothing reorders
// or narrows them, so a read is `data[start + row]`.
template <typename T, typename Nulls>
void ReadCell(sqlite3_context* ctx,
              const Cell& cell,
              uint32_t index,
              const StringPool* pool) {
  if constexpr (!std::is_same_v<Nulls, core::NonNull>) {
    if (!((cell.validity[index / 64] >> (index % 64)) & 1)) {
      sqlite::result::Null(ctx);
      return;
    }
  }
  if constexpr (std::is_same_v<T, core::Id>) {
    sqlite::result::Long(ctx, index);
  } else {
    uint32_t at = index;
    if constexpr (std::is_same_v<Nulls, core::SparseNull>) {
      uint64_t word = cell.validity[index / 64];
      at = cell.prefix[index / 64] +
           static_cast<uint32_t>(
               PERFETTO_POPCOUNT(word & ((1ull << (index % 64)) - 1)));
    }
    T value = static_cast<const T*>(cell.data)[at];
    if constexpr (std::is_same_v<T, StringPool::Id>) {
      if (value.is_null()) {
        sqlite::result::Null(ctx);
      } else {
        sqlite::result::StaticString(ctx, pool->Get(value).c_str());
      }
    } else if constexpr (std::is_same_v<T, double>) {
      sqlite::result::Double(ctx, value);
    } else {
      sqlite::result::Long(ctx, value);
    }
  }
}

template <typename T>
Reader PickReader(const df::Column& column) {
  const auto& n = column.null_storage.nullability();
  if (n.Is<core::NonNull>()) {
    return &ReadCell<T, core::NonNull>;
  }
  if (n.Is<core::DenseNull>()) {
    return &ReadCell<T, core::DenseNull>;
  }
  return &ReadCell<T, core::SparseNull>;
}

std::atomic<uint64_t> g_ticks[kBucketCount];
std::atomic<uint64_t> g_calls[kBucketCount];
const char* const kBucketNames[] = {
    "prepare", "execute",       "next",          "eof",     "column",
    "rowid",   "  cast+narrow", "  first batch", "  refill"};

void PrintTiming() {
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  fprintf(stderr, "DF_TIMING mode=%s\n", Enabled() ? "pipeline" : "interp");
  for (int i = 0; i < kBucketCount; ++i) {
    double ms =
        static_cast<double>(g_ticks[i].load()) * tb.numer / tb.denom / 1e6;
    fprintf(stderr, "DF_TIMING %-14s %9.2f ms %10" PRIu64 " calls\n",
            kBucketNames[i], ms, g_calls[i].load());
  }
}

}  // namespace

const bool g_timing = [] {
  bool on = getenv("DF_TIMING") != nullptr;
  if (on) {
    atexit(PrintTiming);
  }
  return on;
}();

uint64_t Now() {
  return mach_absolute_time();
}

void AddTime(Bucket bucket, uint64_t start) {
  g_ticks[bucket] += mach_absolute_time() - start;
  g_calls[bucket]++;
}

bool Enabled() {
  static bool enabled = [] {
    const char* env = getenv("PERFETTO_DF_PIPELINE");
    bool on = env && env[0] == '2';
    if (on) {
      atexit(PrintCounts);
    }
    return on;
  }();
  return enabled;
}

void CountFilter(bool pipeline) {
  (pipeline ? g_pipeline : g_interpreter)++;
}

#if defined(SPIKE_AUDIT)
namespace {
constexpr int kAuditBase = 1 << 20;
std::vector<std::string>& Rejected() {
  static auto* r = new std::vector<std::string>();
  return *r;
}
std::vector<uint64_t>& RejectedRuns() {
  static auto* r = new std::vector<uint64_t>();
  return *r;
}
std::string Describe(const df::Dataframe& df, const df::LogicalPlan& plan) {
  static const char* kStrategies[] = {"binary",  "set_id",     "small_value",
                                      "range",   "index_list", "linear"};
  static const char* kOps[] = {"=",      "!=",      "<",      "<=",
                               ">",      ">=",      "GLOB",   "REGEXP",
                               "NOTNULL", "ISNULL", "IN"};
  static const char* kKinds[] = {"scan",    "filter", "index", "empty",
                                 "distinct", "reverse", "sort", "minmax",
                                 "limit",   "output"};
  std::string out = "rows=" + std::to_string(df.row_count());
  for (const lp::Operation& op : plan.ops) {
    out += std::string(" ") + kKinds[op.index()];
    if (op.index() == 1) {
      const auto& f = std::get<lp::Filter>(op);
      out += "(" + df.column_names()[f.col] + kOps[f.op.index()] + " " +
             kStrategies[f.strategy.index()] + ")";
    } else if (op.index() == 2) {
      out += "(";
      for (const auto& p : std::get<lp::IndexFilter>(op).predicates) {
        out += df.column_names()[p.col] + kOps[p.op.index()] + ",";
      }
      out += ")";
    }
  }
  return out;
}
struct RejectedFlush {
  ~RejectedFlush() {
    std::vector<std::pair<uint64_t, std::string>> all;
    for (size_t i = 0; i < Rejected().size(); ++i) {
      all.emplace_back(RejectedRuns()[i], Rejected()[i]);
    }
    std::sort(all.rbegin(), all.rend());
    for (const auto& [runs, what] : all) {
      if (runs) {
        fprintf(stderr, "INTERP runs=%" PRIu64 " :: %s\n", runs, what.c_str());
      }
    }
  }
} g_rejected_flush;
}  // namespace

void AuditInterpreterRun(const char* idx_str) {
  if (strncmp(idx_str, "PIPE2:", 6) != 0) {
    return;
  }
  long id = strtol(idx_str + 6, nullptr, 10);
  if (id >= kAuditBase) {
    RejectedRuns()[static_cast<size_t>(id - kAuditBase)]++;
  }
}
#endif

int Register([[maybe_unused]] const df::Dataframe& df, df::LogicalPlan plan) {
  int why = Unlowerable(plan);
  if (why >= 0) {
    g_unlowered[why]++;
#if defined(SPIKE_AUDIT)
    std::lock_guard<std::mutex> lock(g_mutex);
    Rejected().push_back(std::string(kUnlowered[why]) + ": " +
                         Describe(df, plan));
    RejectedRuns().push_back(0);
    return kAuditBase + static_cast<int>(Rejected().size() - 1);
#else
    return -1;
#endif
  }
  g_plans++;
  std::lock_guard<std::mutex> lock(g_mutex);
  Plans().push_back(std::make_unique<df::LogicalPlan>(std::move(plan)));
  return static_cast<int>(Plans().size() - 1);
}

std::string Encode(int id, const std::string& plan) {
  return "PIPE2:" + std::to_string(id) + "|" + plan;
}

const char* Decode(const char* idx_str, int* id) {
  if (strncmp(idx_str, "PIPE2:", 6) != 0) {
    *id = -1;
    return idx_str;
  }
  char* end;
  *id = static_cast<int>(strtol(idx_str + 6, &end, 10));
#if defined(SPIKE_AUDIT)
  if (*id >= kAuditBase) {
    *id = -1;
  }
#endif
  return end + 1;
}

Query::Query(const df::Dataframe& d, int id) : df_(d) {
  const df::LogicalPlan* plan;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    plan = Plans()[static_cast<size_t>(id)].get();
  }
  auto columns = static_cast<uint32_t>(df_.column_names().size());
  uint64_t cols_used = 0;
  prefixes_.reserve(columns);
  for (const lp::Operation& op : plan->ops) {
    switch (op.index()) {
      case 1: {
        const auto& f = std::get<lp::Filter>(op);
        Predicate p{f.col, f.op, f.storage,
                    f.value_index ? static_cast<int>(*f.value_index) : -1};
        if (std::holds_alternative<lp::BinarySearch>(f.strategy)) {
          narrowings_.push_back(
              {p, ex::ViewOfColumn(df_.column(f.col), &prefixes_)});
        } else {
          filters_.push_back(p);
        }
        break;
      }
      case 2: {
        const auto& index = std::get<lp::IndexFilter>(op);
        permutation_ = df_.indexes()[index.index].permutation_vector()->data();
        for (const auto& pr : index.predicates) {
          Predicate p{pr.col, pr.op, pr.storage,
                      pr.value_index ? static_cast<int>(*pr.value_index) : -1};
          narrowings_.push_back(
              {p, ex::ViewOfColumn(df_.column(pr.col), &prefixes_)});
        }
        break;
      }
      case 3:
        empty_ = true;
        break;
      case 9:
        cols_used = std::get<lp::Output>(op).cols_used;
        break;
      default:
        break;
    }
  }
  // By predicate, narrowings first: the binding its value is cast into, if
  // it has one. Pointers into bindings_ are taken only once it stops growing.
  static const CastFilterValueResult kNoValue{CastFilterValueResult::kValid,
                                              CastFilterValueResult::Id{0}};
  std::vector<int> binding_of;
  auto bind = [&](const Predicate& p) {
    // A null check has an argument but compares against no value.
    if (p.value_index >= 0 && !p.op.IsAnyOf<core::NullOp>()) {
      binding_of.push_back(static_cast<int>(bindings_.size()));
      bindings_.emplace_back(Binding{CasterFor(p.storage, p.op),
                                     static_cast<uint32_t>(p.value_index),
                                     IntegerFor(p.storage), kNoValue});
    } else {
      binding_of.push_back(-1);
    }
  };
  for (Narrowing& n : narrowings_) {
    n.narrow = ex::NarrowerFor(n.view, n.predicate.op, permutation_ != nullptr);
    bind(n.predicate);
  }
  for (const Predicate& p : filters_) {
    bind(p);
  }
  auto value_of = [&](size_t at) -> const CastFilterValueResult* {
    return binding_of[at] < 0 ? &kNoValue : &bindings_[static_cast<size_t>(binding_of[at])].value;
  };

  // The columns a batch carries: those read, then those filtered on.
  Cell unset;
  unset.position = std::numeric_limits<uint32_t>::max();
  cells_.assign(columns, unset);
  std::vector<uint32_t> emitted;
  auto emit = [&](uint32_t c) {
    if (cells_[c].position == std::numeric_limits<uint32_t>::max()) {
      cells_[c].position = static_cast<uint32_t>(emitted.size());
      emitted.push_back(c);
    }
  };
  for (uint32_t c = 0; c < columns; ++c) {
    if ((cols_used >> std::min(c, 63u)) & 1) {
      emit(c);
    }
  }
  for (const Predicate& p : filters_) {
    emit(p.column);
  }
  for (uint32_t c = 0; c < columns; ++c) {
    if (df_.column(c).storage.type().Is<core::Id>()) {
      emit(c);
    }
  }
  for (uint32_t c : emitted) {
    const df::Column& column = df_.column(c);
    ex::ColumnView view = ex::ViewOfColumn(column, &prefixes_);
    cells_[c].data = view.data();
    cells_[c].validity = view.validity() ? view.validity()->words() : nullptr;
    cells_[c].prefix = view.prefix();
    Reader& read = cells_[c].read;
    switch (column.storage.type().index()) {
      case StorageType::GetTypeIndex<core::Id>():
        read = PickReader<core::Id>(column);
        break;
      case StorageType::GetTypeIndex<core::Uint32>():
        read = PickReader<uint32_t>(column);
        break;
      case StorageType::GetTypeIndex<core::Int32>():
        read = PickReader<int32_t>(column);
        break;
      case StorageType::GetTypeIndex<core::Int64>():
        read = PickReader<int64_t>(column);
        break;
      case StorageType::GetTypeIndex<core::Double>():
        read = PickReader<double>(column);
        break;
      default:
        read = PickReader<StringPool::Id>(column);
        break;
    }
  }

  std::vector<ex::RowsScan::Narrowing> narrowings;
  for (size_t i = 0; i < narrowings_.size(); ++i) {
    const Narrowing& n = narrowings_[i];
    narrowings.push_back({n.narrow, value_of(i), n.predicate.op, n.view});
  }
  scan_ = std::make_unique<ex::RowsScan>(df_, emitted, permutation_,
                                         std::move(narrowings), empty_);
  std::vector<std::unique_ptr<ex::Operator>> ops;
  for (size_t i = 0; i < filters_.size(); ++i) {
    ops.push_back(std::make_unique<ex::ColumnFilter>(
        cells_[filters_[i].column].position, filters_[i].storage,
        filters_[i].op,
        value_of(narrowings_.size() + i), df_.string_pool()));
  }
  // A pipeline with no operators is its source.
  if (ops.empty()) {
    runner_ = scan_.get();
  } else {
    pipeline_ = std::make_unique<ex::Pipeline>(*scan_, std::move(ops),
                                               ex::ExecutionOptions());
    runner_ = pipeline_.get();
  }
  state_ = runner_->MakeState();
#if defined(SPIKE_AUDIT)
  {
    auto name = [&](uint32_t c) { return df_.column_names()[c]; };
    auto op = [](core::Op o) {
      static const char* kOps[] = {"=", "!=", "<", "<=", ">", ">=", "GLOB",
                                   "REGEXP", "NOTNULL", "ISNULL", "IN"};
      return kOps[o.index()];
    };
    std::string plan = "rows=" + std::to_string(df_.row_count());
    plan += permutation_ ? " index" : "";
    for (const Narrowing& n : narrowings_) {
      plan += " narrow(" + name(n.predicate.column) + op(n.predicate.op) + ")";
    }
    for (const Predicate& p : filters_) {
      plan += " filter(" + name(p.column) + op(p.op) + ")";
    }
    plan += " cols=" + std::to_string(emitted.size());
    plan += " [" + name(0) + "," + name(1) + ",...]";
    audit_plan_ = plan;
  }
#endif
  run_state_ = state_.get();
  cells_data_ = cells_.data();
  pool_ = df_.string_pool();
}

#if defined(SPIKE_AUDIT)
namespace {
struct AuditLog {
  ~AuditLog() {
    std::vector<std::pair<std::string, Query::Audit>> all(plans.begin(),
                                                           plans.end());
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
      return a.second.rows + a.second.executes * 8 >
             b.second.rows + b.second.executes * 8;
    });
    for (const auto& [plan, a] : all) {
      fprintf(stderr,
              "AUDIT executes=%" PRIu64 " batches=%" PRIu64 " rows=%" PRIu64
              " :: %s\n",
              a.executes, a.batches, a.rows, plan.c_str());
    }
  }
  std::map<std::string, Query::Audit> plans;
};
AuditLog& Log() {
  static AuditLog* log = new AuditLog();
  return *log;
}
struct AuditFlush {
  ~AuditFlush() { Log().~AuditLog(); }
} g_audit_flush;
}  // namespace

Query::~Query() {
  Query::Audit& a = Log().plans[audit_plan_];
  a.executes += audit_.executes;
  a.batches += audit_.batches;
  a.rows += audit_.rows;
}
#else
Query::~Query() = default;
#endif

// Casts an integer argument in range of `integer` into `out`; returns false,
// leaving `out` as it was, for anything else.
PERFETTO_ALWAYS_INLINE bool CastInteger(IntegerCast integer,
                                        sqlite3_value* value,
                                        CastFilterValueResult* out) {
  if (integer == IntegerCast::kNone ||
      sqlite::value::Type(value) != sqlite::Type::kInteger) {
    return false;
  }
  int64_t v = sqlite::value::Int64(value);
  auto in = [v](auto type) {
    using T = decltype(type);
    return v >= std::numeric_limits<T>::min() &&
           v <= std::numeric_limits<T>::max();
  };
  switch (integer) {
    case IntegerCast::kId:
      if (!in(uint32_t{})) {
        return false;
      }
      out->value.emplace<CastFilterValueResult::Id>(
          CastFilterValueResult::Id{static_cast<uint32_t>(v)});
      break;
    case IntegerCast::kUint32:
      if (!in(uint32_t{})) {
        return false;
      }
      out->value.emplace<uint32_t>(static_cast<uint32_t>(v));
      break;
    case IntegerCast::kInt32:
      if (!in(int32_t{})) {
        return false;
      }
      out->value.emplace<int32_t>(static_cast<int32_t>(v));
      break;
    case IntegerCast::kInt64:
      out->value.emplace<int64_t>(v);
      break;
    case IntegerCast::kNone:
      return false;
  }
  out->validity = CastFilterValueResult::kValid;
  return true;
}

void Query::Execute(sqlite3_value** argv) {
  for (Binding& b : bindings_) {
    if (PERFETTO_UNLIKELY(!CastInteger(b.integer, argv[b.index], &b.value))) {
      b.cast(b.index, argv, &b.value);
    }
  }
  // Opening the pipeline opens the scan, which narrows by the values cast.
  row_ = 0;
  batch_ = runner_->Open(scratch_, *run_state_);
  Arrived();
#if defined(SPIKE_AUDIT)
  audit_.executes++;
  audit_.batches += batch_ != nullptr;
  audit_.rows += count_;
#endif
}

void Query::Advance() {
  row_ = 0;
  batch_ = runner_->Next(scratch_, *run_state_);
  Arrived();
#if defined(SPIKE_AUDIT)
  audit_.batches += batch_ != nullptr;
  audit_.rows += count_;
#endif
}


}  // namespace perfetto::trace_processor::pipeline_spike2
