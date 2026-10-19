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

#include "src/trace_processor/core/exec/expression.h"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using Op = ExprStep::Op;

// The value of one step at every kept row of a batch: batch row r's value is
// at `start + r` of `data`, present unless `validity` is set and that bit is
// clear. A register read straight from a batch column shares its storage.
struct Register {
  const void* data = nullptr;
  const BitVector* validity = nullptr;
  uint32_t start = 0;
  // The storage the register is in, null if it borrows storage which
  // outlives the run.
  ColumnBuffer buffer;

  template <typename T>
  PERFETTO_ALWAYS_INLINE const T* Values() const {
    return static_cast<const T*>(data) + start;
  }
  PERFETTO_ALWAYS_INLINE bool Valid(uint32_t row) const {
    return !validity || validity->is_set(start + row);
  }
};

template <typename F>
PERFETTO_ALWAYS_INLINE void ForEachRow(const Selection& sel, F f) {
  uint32_t n = sel.size();
  if (sel.prefix()) {
    for (uint32_t r = 0; r < n; ++r) {
      f(r);
    }
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      f(sel[i]);
    }
  }
}

StorageType StorageFor(ExprType type) {
  switch (type) {
    case ExprType::kInt64:
      return Int64{};
    case ExprType::kDouble:
      return Double{};
    case ExprType::kString:
      return String{};
  }
  PERFETTO_FATAL("For GCC");
}

// Runs one program over batches, keeping a register per step.
class Evaluator {
 public:
  Evaluator(const ExprProgram& program, Context& context)
      : program_(&program), context_(&context) {
    regs_.resize(program.steps.size());
    constants_.resize(program.steps.size());
    for (uint32_t i = 0; i < program.steps.size(); ++i) {
      const ExprStep& step = program.steps[i];
      if (step.op == Op::kConstant || step.op == Op::kNull) {
        FillConstant(step, constants_[i]);
      }
    }
  }

  // Computes every step at the kept rows of `batch`. False if the program
  // failed, with `status` saying why.
  bool Run(const RowBatch& batch, base::Status* status) {
    const Selection& sel = batch.selection();
    for (uint32_t i = 0; i < program_->steps.size(); ++i) {
      const ExprStep& step = program_->steps[i];
      Register& out = regs_[i];
      out = Register();
      if (!RunStep(step, batch, sel, out)) {
        *status = base::ErrStatus("integer overflow");
        return false;
      }
    }
    return true;
  }

  const Register& result() const { return regs_.back(); }
  ExprType result_type() const { return program_->steps.back().type; }

 private:
  template <typename T>
  T* Alloc(Register& out) {
    out.buffer = context_->TakeBuffer();
    out.validity = nullptr;
    out.start = 0;
    T* values = out.buffer.chunk().Values<T>();
    out.data = values;
    return values;
  }

  // The register's own validity, sized for any batch. Call after Alloc.
  static BitVector* AllocValidity(Register& out) {
    BitVector& validity = out.buffer.chunk().validity;
    if (validity.size() != kMaxBatchRows) {
      validity.resize(kMaxBatchRows);
    }
    out.validity = &validity;
    return &validity;
  }

  // Sets `out` present where `a` is.
  static void CopyValidity(const Selection& sel,
                           const Register& a,
                           Register& out) {
    if (!a.validity) {
      return;
    }
    BitVector* v = AllocValidity(out);
    ForEachRow(sel, [&](uint32_t r) { v->change(r, a.Valid(r)); });
  }

  // Sets `out` present where both `a` and `b` are.
  static void BothValid(const Selection& sel,
                        const Register& a,
                        const Register& b,
                        Register& out) {
    if (!a.validity && !b.validity) {
      return;
    }
    BitVector* v = AllocValidity(out);
    ForEachRow(sel,
               [&](uint32_t r) { v->change(r, a.Valid(r) && b.Valid(r)); });
  }

  void FillConstant(const ExprStep& step, Register& out) {
    switch (step.type) {
      case ExprType::kInt64: {
        int64_t* v = Alloc<int64_t>(out);
        for (uint32_t r = 0; r < kMaxBatchRows; ++r) {
          v[r] = step.int_value;
        }
        break;
      }
      case ExprType::kDouble: {
        double* v = Alloc<double>(out);
        for (uint32_t r = 0; r < kMaxBatchRows; ++r) {
          v[r] = step.double_value;
        }
        break;
      }
      case ExprType::kString: {
        StringPool::Id* v = Alloc<StringPool::Id>(out);
        for (uint32_t r = 0; r < kMaxBatchRows; ++r) {
          v[r] = step.string_value;
        }
        break;
      }
    }
    if (step.op == Op::kNull) {
      AllocValidity(out)->ClearAllBits();
    }
  }

  void LoadColumn(const ColumnView& column,
                  const ColumnBuffer& buffer,
                  const Selection& sel,
                  Register& out) {
    PERFETTO_CHECK(column.kind() != ColumnView::Kind::kVariant);
    if (column.kind() == ColumnView::Kind::kSequence) {
      int64_t* v = Alloc<int64_t>(out);
      uint32_t start = column.start();
      ForEachRow(sel, [&](uint32_t r) { v[r] = start + r; });
      Register in;
      in.validity = column.validity();
      in.start = start;
      CopyValidity(sel, in, out);
      return;
    }
    StorageType type = column.type();
    if (type.Is<Int64>() || type.Is<Double>() || type.Is<String>()) {
      out.data = column.data();
      out.validity = column.validity();
      out.start = column.start();
      out.buffer = buffer;
      return;
    }
    Register in;
    in.data = column.data();
    in.validity = column.validity();
    in.start = column.start();
    int64_t* v = Alloc<int64_t>(out);
    if (type.Is<Uint32>()) {
      const uint32_t* x = in.Values<uint32_t>();
      ForEachRow(sel, [&](uint32_t r) { v[r] = x[r]; });
    } else {
      PERFETTO_CHECK(type.Is<Int32>());
      const int32_t* x = in.Values<int32_t>();
      ForEachRow(sel, [&](uint32_t r) { v[r] = x[r]; });
    }
    CopyValidity(sel, in, out);
  }

  // `f(x, y, &out)` returns whether it overflowed. Only rows holding values
  // count, so the check is redone with nulls only if something overflowed.
  template <typename F>
  bool IntArith(const Selection& sel,
                const Register& a,
                const Register& b,
                Register& out,
                F f) {
    const int64_t* x = a.Values<int64_t>();
    const int64_t* y = b.Values<int64_t>();
    int64_t* o = Alloc<int64_t>(out);
    BothValid(sel, a, b, out);
    bool overflow = false;
    ForEachRow(sel, [&](uint32_t r) { overflow |= f(x[r], y[r], &o[r]); });
    if (PERFETTO_LIKELY(!overflow)) {
      return true;
    }
    bool real = false;
    ForEachRow(sel, [&](uint32_t r) {
      int64_t unused;
      real |= out.Valid(r) && f(x[r], y[r], &unused);
    });
    return !real;
  }

  template <typename F>
  void DoubleArith(const Selection& sel,
                   const Register& a,
                   const Register& b,
                   Register& out,
                   F f) {
    const double* x = a.Values<double>();
    const double* y = b.Values<double>();
    double* o = Alloc<double>(out);
    BothValid(sel, a, b, out);
    ForEachRow(sel, [&](uint32_t r) { o[r] = f(x[r], y[r]); });
  }

  // Division or remainder, null where the divisor is zero.
  template <typename T>
  bool Divide(const Selection& sel,
              const Register& a,
              const Register& b,
              bool remainder,
              Register& out) {
    const T* x = a.Values<T>();
    const T* y = b.Values<T>();
    T* o = Alloc<T>(out);
    BitVector* v = AllocValidity(out);
    bool overflow = false;
    ForEachRow(sel, [&](uint32_t r) {
      bool ok = a.Valid(r) && b.Valid(r) && !std::equal_to<T>()(y[r], T(0));
      v->change(r, ok);
      if (!ok) {
        o[r] = 0;
        return;
      }
      if constexpr (std::is_same_v<T, int64_t>) {
        if (y[r] == -1) {
          // INT64_MIN / -1 is the one quotient which does not fit.
          overflow |= !remainder && x[r] == std::numeric_limits<T>::min();
          o[r] = remainder ? 0
                           : static_cast<T>(0ull - static_cast<uint64_t>(x[r]));
          return;
        }
        o[r] = remainder ? x[r] % y[r] : x[r] / y[r];
      } else {
        o[r] = x[r] / y[r];
      }
    });
    return !overflow;
  }

  template <typename T, typename Cmp>
  void Compare(const Selection& sel,
               const Register& a,
               const Register& b,
               Register& out,
               Cmp cmp) {
    const T* x = a.Values<T>();
    const T* y = b.Values<T>();
    int64_t* o = Alloc<int64_t>(out);
    BothValid(sel, a, b, out);
    ForEachRow(sel, [&](uint32_t r) { o[r] = cmp(x[r], y[r]); });
  }

  template <typename T>
  void CompareByOp(Op op,
                   const Selection& sel,
                   const Register& a,
                   const Register& b,
                   Register& out) {
    if (op == Op::kEq) {
      Compare<T>(sel, a, b, out, std::equal_to<T>());
    } else if (op == Op::kNe) {
      Compare<T>(sel, a, b, out, std::not_equal_to<T>());
    } else if (op == Op::kLt) {
      Compare<T>(sel, a, b, out, std::less<T>());
    } else if (op == Op::kLe) {
      Compare<T>(sel, a, b, out, std::less_equal<T>());
    } else if (op == Op::kGt) {
      Compare<T>(sel, a, b, out, std::greater<T>());
    } else {
      PERFETTO_DCHECK(op == Op::kGe);
      Compare<T>(sel, a, b, out, std::greater_equal<T>());
    }
  }

  // AND and OR by SQL's three valued logic: a known false operand decides AND
  // and a known true one decides OR, whatever the other is.
  static void Logic(const Selection& sel,
                    const Register& a,
                    const Register& b,
                    bool is_and,
                    int64_t* o,
                    Register& out) {
    const int64_t* x = a.Values<int64_t>();
    const int64_t* y = b.Values<int64_t>();
    if (!a.validity && !b.validity) {
      if (is_and) {
        ForEachRow(sel, [&](uint32_t r) { o[r] = x[r] != 0 && y[r] != 0; });
      } else {
        ForEachRow(sel, [&](uint32_t r) { o[r] = x[r] != 0 || y[r] != 0; });
      }
      return;
    }
    BitVector* v = AllocValidity(out);
    ForEachRow(sel, [&](uint32_t r) {
      bool av = a.Valid(r);
      bool bv = b.Valid(r);
      // The value which decides the result on its own.
      bool decisive = !is_and;
      bool decided =
          (av && (x[r] != 0) == decisive) || (bv && (y[r] != 0) == decisive);
      if (decided) {
        o[r] = decisive;
        v->set(r);
      } else if (av && bv) {
        o[r] = !decisive;
        v->set(r);
      } else {
        o[r] = 0;
        v->clear(r);
      }
    });
  }

  // False on overflow.
  bool RunStep(const ExprStep& step,
               const RowBatch& batch,
               const Selection& sel,
               Register& out) {
    switch (step.op) {
      case Op::kColumn:
        LoadColumn(batch.column(step.a), batch.buffer(step.a), sel, out);
        return true;
      case Op::kConstant:
      case Op::kNull:
        out = constants_[static_cast<size_t>(&step - program_->steps.data())];
        return true;
      case Op::kNeg: {
        const Register& a = regs_[step.a];
        if (step.type == ExprType::kDouble) {
          const double* x = a.Values<double>();
          double* o = Alloc<double>(out);
          CopyValidity(sel, a, out);
          ForEachRow(sel, [&](uint32_t r) { o[r] = -x[r]; });
          return true;
        }
        const int64_t* x = a.Values<int64_t>();
        int64_t* o = Alloc<int64_t>(out);
        CopyValidity(sel, a, out);
        bool overflow = false;
        ForEachRow(sel, [&](uint32_t r) {
          overflow |= out.Valid(r) && __builtin_sub_overflow(0, x[r], &o[r]);
        });
        return !overflow;
      }
      case Op::kNot: {
        const Register& a = regs_[step.a];
        const int64_t* x = a.Values<int64_t>();
        int64_t* o = Alloc<int64_t>(out);
        CopyValidity(sel, a, out);
        ForEachRow(sel, [&](uint32_t r) { o[r] = x[r] == 0; });
        return true;
      }
      case Op::kToDouble: {
        const Register& a = regs_[step.a];
        const int64_t* x = a.Values<int64_t>();
        double* o = Alloc<double>(out);
        CopyValidity(sel, a, out);
        ForEachRow(sel, [&](uint32_t r) { o[r] = static_cast<double>(x[r]); });
        return true;
      }
      case Op::kAdd:
      case Op::kSub:
      case Op::kMul:
        return Arith(step, sel, out);
      case Op::kDiv:
      case Op::kMod: {
        const Register& a = regs_[step.a];
        const Register& b = regs_[step.b];
        if (step.type == ExprType::kDouble) {
          PERFETTO_DCHECK(step.op == Op::kDiv);
          return Divide<double>(sel, a, b, false, out);
        }
        return Divide<int64_t>(sel, a, b, step.op == Op::kMod, out);
      }
      case Op::kEq:
      case Op::kNe:
      case Op::kLt:
      case Op::kLe:
      case Op::kGt:
      case Op::kGe: {
        const Register& a = regs_[step.a];
        const Register& b = regs_[step.b];
        switch (step.operand_type) {
          case ExprType::kInt64:
            CompareByOp<int64_t>(step.op, sel, a, b, out);
            break;
          case ExprType::kDouble:
            CompareByOp<double>(step.op, sel, a, b, out);
            break;
          case ExprType::kString:
            // Equal strings are the same string in the pool.
            PERFETTO_DCHECK(step.op == Op::kEq || step.op == Op::kNe);
            CompareByOp<uint32_t>(step.op, sel, a, b, out);
            break;
        }
        return true;
      }
      case Op::kAnd:
      case Op::kOr: {
        int64_t* o = Alloc<int64_t>(out);
        Logic(sel, regs_[step.a], regs_[step.b], step.op == Op::kAnd, o, out);
        return true;
      }
      case Op::kIsNull:
      case Op::kIsNotNull: {
        const Register& a = regs_[step.a];
        int64_t* o = Alloc<int64_t>(out);
        bool want = step.op == Op::kIsNotNull;
        ForEachRow(sel, [&](uint32_t r) { o[r] = a.Valid(r) == want; });
        return true;
      }
    }
    PERFETTO_FATAL("For GCC");
  }

  bool Arith(const ExprStep& step, const Selection& sel, Register& out) {
    const Register& a = regs_[step.a];
    const Register& b = regs_[step.b];
    if (step.type == ExprType::kDouble) {
      if (step.op == Op::kAdd) {
        DoubleArith(sel, a, b, out, [](double x, double y) { return x + y; });
      } else if (step.op == Op::kSub) {
        DoubleArith(sel, a, b, out, [](double x, double y) { return x - y; });
      } else {
        DoubleArith(sel, a, b, out, [](double x, double y) { return x * y; });
      }
      return true;
    }
    if (step.op == Op::kAdd) {
      return IntArith(sel, a, b, out, [](int64_t x, int64_t y, int64_t* o) {
        return __builtin_add_overflow(x, y, o);
      });
    }
    if (step.op == Op::kSub) {
      return IntArith(sel, a, b, out, [](int64_t x, int64_t y, int64_t* o) {
        return __builtin_sub_overflow(x, y, o);
      });
    }
    return IntArith(sel, a, b, out, [](int64_t x, int64_t y, int64_t* o) {
      return __builtin_mul_overflow(x, y, o);
    });
  }

  const ExprProgram* program_;
  Context* context_;
  std::vector<Register> regs_;
  // By step, the register of each constant step, filled for every row once.
  std::vector<Register> constants_;
};

struct ComputeState : OperatorState {
  ~ComputeState() override;
  std::vector<Evaluator> evaluators;
  base::Status status = base::OkStatus();
};
ComputeState::~ComputeState() = default;

struct WhereState : OperatorState {
  WhereState(const ExprProgram& program, Context& context)
      : evaluator(program, context) {}
  ~WhereState() override;
  Evaluator evaluator;
  base::Status status = base::OkStatus();
  // The batch rows kept, increasing.
  std::array<uint32_t, kMaxBatchRows> rows;
};
WhereState::~WhereState() = default;

}  // namespace

Compute::Compute(std::vector<ExprProgram> programs)
    : programs_(std::move(programs)) {}
Compute::~Compute() = default;

std::unique_ptr<OperatorState> Compute::MakeState(Context& context) const {
  auto state = std::make_unique<ComputeState>();
  state->evaluators.reserve(programs_.size());
  for (const ExprProgram& program : programs_) {
    state->evaluators.emplace_back(program, context);
  }
  return state;
}

bool Compute::Process(RowBatch& batch, OperatorState& state) const {
  auto& s = state.Cast<ComputeState>();
  for (Evaluator& evaluator : s.evaluators) {
    if (!evaluator.Run(batch, &s.status)) {
      return false;
    }
  }
  // Added only once all have run: each reads the batch as it came.
  for (Evaluator& evaluator : s.evaluators) {
    const Register& result = evaluator.result();
    batch.AddColumn(
        ColumnView::Reference(StorageFor(evaluator.result_type()), result.data,
                              result.validity, result.start),
        result.buffer);
  }
  return true;
}

base::Status Compute::status(const OperatorState& state) const {
  return state.Cast<ComputeState>().status;
}

Where::Where(ExprProgram program) : program_(std::move(program)) {}
Where::~Where() = default;

std::unique_ptr<OperatorState> Where::MakeState(Context& context) const {
  return std::make_unique<WhereState>(program_, context);
}

bool Where::Process(RowBatch& batch, OperatorState& state) const {
  auto& s = state.Cast<WhereState>();
  if (!s.evaluator.Run(batch, &s.status)) {
    return false;
  }
  const Register& result = s.evaluator.result();
  const int64_t* x = result.Values<int64_t>();
  uint32_t kept = 0;
  ForEachRow(batch.selection(), [&](uint32_t r) {
    s.rows[kept] = r;
    kept += result.Valid(r) && x[r] != 0;
  });
  batch.mutable_selection().KeepRows(
      Span<const uint32_t>(s.rows.data(), s.rows.data() + kept));
  return true;
}

base::Status Where::status(const OperatorState& state) const {
  return state.Cast<WhereState>().status;
}

}  // namespace perfetto::trace_processor::core::exec
