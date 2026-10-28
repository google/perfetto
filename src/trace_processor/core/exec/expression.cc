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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using Op = ExprStep::Op;

// The value of one step at every row of a batch: batch row r's value is at
// `start + r` of `data`, present unless `validity` is set and that bit is
// clear. A register read straight from a batch column shares its storage.
struct Register {
  const void* data = nullptr;
  const BitVector* validity = nullptr;
  uint32_t start = 0;
  // The storage the register is in, null if it borrows storage which
  // outlives the run.
  ColumnBuffer buffer;

  template <typename T>
  const T* Values() const {
    return static_cast<const T*>(data) + start;
  }
  bool Valid(uint32_t row) const {
    return !validity || validity->is_set(start + row);
  }
};

// Integer division or remainder by a divisor which is not zero. False if the
// result does not fit, which only INT64_MIN / -1 does not.
bool Div(int64_t x, int64_t y, bool remainder, int64_t* o) {
  if (y == -1) {
    *o = 0;
    return remainder || base::CheckedSub(0, x, o);
  }
  *o = remainder ? x % y : x / y;
  return true;
}

// Whether the integer `op` fails at some row the batch keeps where `out`
// holds a value. The loops compute every row, so this only runs once one of
// them has failed somewhere.
PERFETTO_NO_INLINE bool KeptRowFails(Op op,
                                     const int64_t* x,
                                     const int64_t* y,
                                     const Register& out,
                                     const Selection& selection) {
  for (uint32_t i = 0; i < selection.size(); ++i) {
    uint32_t r = selection[i];
    if (!out.Valid(r)) {
      continue;
    }
    int64_t o;
    bool fits = op == Op::kAdd   ? base::CheckedAdd(x[r], y[r], &o)
                : op == Op::kSub ? base::CheckedSub(x[r], y[r], &o)
                : op == Op::kMul ? base::CheckedMul(x[r], y[r], &o)
                                 : Div(x[r], y[r], op == Op::kMod, &o);
    if (!fits) {
      return true;
    }
  }
  return false;
}

// The loops, one per operation and type, kept to the arithmetic itself so
// that each copy is small: everything else is shared. Each returns false if
// any row did not fit, checking once at the end rather than at each row.
template <typename T, typename F>
PERFETTO_NO_INLINE bool Loop(const T* x, const T* y, T* o, uint32_t rows, F f) {
  bool fits = true;
  for (uint32_t r = 0; r < rows; ++r) {
    fits &= f(x[r], y[r], &o[r]);
  }
  return fits;
}

// As Loop, for an operation which vector instructions do not speed up, such as
// a checked multiplication or a division.
template <typename F>
PERFETTO_NO_INLINE bool ScalarLoop(const int64_t* x,
                                   const int64_t* y,
                                   int64_t* o,
                                   uint32_t rows,
                                   F f) {
  bool fits = true;
  PERFETTO_NO_VECTORIZE_LOOP
  for (uint32_t r = 0; r < rows; ++r) {
    fits &= f(x[r], y[r], &o[r]);
  }
  return fits;
}

template <typename From, typename To>
PERFETTO_NO_INLINE void Convert(const From* x, To* o, uint32_t rows) {
  for (uint32_t r = 0; r < rows; ++r) {
    o[r] = static_cast<To>(x[r]);
  }
}

// Runs once per run rather than per batch, so is kept small rather than fast.
template <typename T>
PERFETTO_NO_INLINE void Fill(T* o, T value) {
  PERFETTO_NO_VECTORIZE_LOOP
  for (uint32_t r = 0; r < kMaxBatchRows; ++r) {
    o[r] = value;
  }
}

PERFETTO_NO_INLINE void Sequence(uint32_t start, int64_t* o, uint32_t rows) {
  for (uint32_t r = 0; r < rows; ++r) {
    o[r] = start + r;
  }
}

bool IntArithmetic(Op op,
                   const int64_t* x,
                   const int64_t* y,
                   int64_t* o,
                   uint32_t rows) {
  switch (op) {
    case Op::kAdd:
      return Loop(x, y, o, rows, [](int64_t a, int64_t b, int64_t* c) {
        return base::CheckedAdd(a, b, c);
      });
    case Op::kSub:
      return Loop(x, y, o, rows, [](int64_t a, int64_t b, int64_t* c) {
        return base::CheckedSub(a, b, c);
      });
    case Op::kMul:
      return ScalarLoop(x, y, o, rows, [](int64_t a, int64_t b, int64_t* c) {
        return base::CheckedMul(a, b, c);
      });
    case Op::kDiv:
    case Op::kMod: {
      // A zero divisor gives null, so any value will do for its row.
      bool remainder = op == Op::kMod;
      return ScalarLoop(x, y, o, rows,
                        [remainder](int64_t a, int64_t b, int64_t* c) {
                          return Div(a, b == 0 ? 1 : b, remainder, c);
                        });
    }
    case Op::kColumn:
    case Op::kConstant:
    case Op::kNull:
    case Op::kToDouble:
      break;
  }
  PERFETTO_FATAL("Not arithmetic");
}

void DoubleArithmetic(Op op,
                      const double* x,
                      const double* y,
                      double* o,
                      uint32_t rows) {
  switch (op) {
    case Op::kAdd:
      Loop(x, y, o, rows, [](double a, double b, double* c) {
        *c = a + b;
        return true;
      });
      return;
    case Op::kSub:
      Loop(x, y, o, rows, [](double a, double b, double* c) {
        *c = a - b;
        return true;
      });
      return;
    case Op::kMul:
      Loop(x, y, o, rows, [](double a, double b, double* c) {
        *c = a * b;
        return true;
      });
      return;
    case Op::kDiv:
      Loop(x, y, o, rows, [](double a, double b, double* c) {
        *c = a / b;
        return true;
      });
      return;
    case Op::kMod:
    case Op::kColumn:
    case Op::kConstant:
    case Op::kNull:
    case Op::kToDouble:
      break;
  }
  PERFETTO_FATAL("Not double arithmetic");
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
      if (step.op != Op::kConstant && step.op != Op::kNull) {
        continue;
      }
      Register& out = constants_[i];
      if (step.type == ExprType::kDouble) {
        Fill(Alloc<double>(out), step.double_value);
      } else {
        PERFETTO_DCHECK(step.type == ExprType::kInt64);
        Fill(Alloc<int64_t>(out), step.int_value);
      }
      if (step.op == Op::kNull) {
        AllocValidity(out)->ClearAllBits();
      }
    }
  }

  // Computes every step for `batch`. False if the program failed, with
  // `status` saying why.
  bool Run(const RowBatch& batch, base::Status* status) {
    rows_ = batch.row_count();
    for (uint32_t i = 0; i < program_->steps.size(); ++i) {
      regs_[i] = Register();
      if (!RunStep(program_->steps[i], batch, regs_[i])) {
        *status = base::ErrStatus("integer overflow");
        return false;
      }
    }
    return true;
  }

  const Register& result() const { return regs_.back(); }
  ExprType result_type() const { return program_->steps.back().type; }

 private:
  // A buffer for `out` to hold its values in, with no validity yet.
  template <typename T>
  T* Alloc(Register& out) {
    return static_cast<T*>(AllocBuffer(out, sizeof(T)));
  }
  PERFETTO_NO_INLINE void* AllocBuffer(Register& out, uint32_t width) {
    out.buffer = context_->TakeBuffer(width);
    out.validity = nullptr;
    out.start = 0;
    uint8_t* values = out.buffer.chunk().Values<uint8_t>();
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

  // Sets `out` present where `a` and, if given, `b` are, and for a division
  // where `b` is not zero. Leaves `out` without validity if it would hold
  // every row.
  void Validity(const Register& a,
                const Register* b,
                bool division,
                bool doubles,
                Register& out) const {
    if (!division && !a.validity && !(b && b->validity)) {
      return;
    }
    BitVector* v = AllocValidity(out);
    v->FillBits(0, rows_, true);
    for (const Register* in : {&a, b}) {
      if (in && in->validity) {
        v->AndBitsFrom(0, *in->validity, in->start, rows_);
      }
    }
    if (!division) {
      return;
    }
    // Division by zero is null, as in SQLite.
    if (doubles) {
      const double* y = b->Values<double>();
      for (uint32_t r = 0; r < rows_; ++r) {
        if (base::IsZero(y[r])) {
          v->clear(r);
        }
      }
    } else {
      const int64_t* y = b->Values<int64_t>();
      for (uint32_t r = 0; r < rows_; ++r) {
        if (y[r] == 0) {
          v->clear(r);
        }
      }
    }
  }

  void LoadColumn(const ColumnView& column,
                  const ColumnBuffer& buffer,
                  Register& out) {
    PERFETTO_CHECK(column.kind() != ColumnView::Kind::kVariant);
    StorageType type = column.type();
    if (column.kind() == ColumnView::Kind::kFlat &&
        (type.Is<Int64>() || type.Is<Double>() || type.Is<String>())) {
      out.data = column.data();
      out.validity = column.validity();
      out.start = column.start();
      out.buffer = buffer;
      return;
    }
    int64_t* o = Alloc<int64_t>(out);
    if (column.kind() == ColumnView::Kind::kSequence) {
      Sequence(column.start(), o, rows_);
    } else if (type.Is<Uint32>()) {
      Convert(static_cast<const uint32_t*>(column.data()) + column.start(), o,
              rows_);
    } else {
      PERFETTO_CHECK(type.Is<Int32>());
      Convert(static_cast<const int32_t*>(column.data()) + column.start(), o,
              rows_);
    }
    Register in;
    in.validity = column.validity();
    in.start = column.start();
    Validity(in, nullptr, false, false, out);
  }

  // False if an integer result did not fit at a row the batch keeps.
  bool Arithmetic(const ExprStep& step,
                  const Selection& selection,
                  Register& out) {
    const Register& a = regs_[step.a];
    const Register& b = regs_[step.b];
    bool division = step.op == Op::kDiv || step.op == Op::kMod;
    bool doubles = step.type == ExprType::kDouble;
    if (doubles) {
      double* o = Alloc<double>(out);
      Validity(a, &b, division, doubles, out);
      DoubleArithmetic(step.op, a.Values<double>(), b.Values<double>(), o,
                       rows_);
      return true;
    }
    int64_t* o = Alloc<int64_t>(out);
    Validity(a, &b, division, doubles, out);
    const int64_t* x = a.Values<int64_t>();
    const int64_t* y = b.Values<int64_t>();
    return IntArithmetic(step.op, x, y, o, rows_) ||
           !KeptRowFails(step.op, x, y, out, selection);
  }

  // False if the step failed.
  bool RunStep(const ExprStep& step, const RowBatch& batch, Register& out) {
    switch (step.op) {
      case Op::kColumn:
        LoadColumn(batch.column(step.a), batch.buffer(step.a), out);
        return true;
      case Op::kConstant:
      case Op::kNull:
        out = constants_[static_cast<size_t>(&step - program_->steps.data())];
        return true;
      case Op::kToDouble: {
        const Register& a = regs_[step.a];
        double* o = Alloc<double>(out);
        Validity(a, nullptr, false, false, out);
        Convert(a.Values<int64_t>(), o, rows_);
        return true;
      }
      case Op::kAdd:
      case Op::kSub:
      case Op::kMul:
      case Op::kDiv:
      case Op::kMod:
        return Arithmetic(step, batch.selection(), out);
    }
    PERFETTO_FATAL("For GCC");
  }

  const ExprProgram* program_;
  Context* context_;
  std::vector<Register> regs_;
  // By step, the register of each constant step, filled for every row once.
  std::vector<Register> constants_;
  // The number of rows of the batch being run.
  uint32_t rows_ = 0;
};

struct ComputeState : OperatorState {
  ~ComputeState() override;
  std::vector<Evaluator> evaluators;
  base::Status status = base::OkStatus();
};
ComputeState::~ComputeState() = default;

}  // namespace

void Compute::DescribeColumns(StepColumns* out) const {
  for (const ExprProgram& program : programs_) {
    for (const ExprStep& step : program.steps) {
      if (step.op == ExprStep::Op::kColumn) {
        out->reads.push_back(step.a);
      }
    }
  }
  out->added = static_cast<uint32_t>(programs_.size());
}

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
  for (auto& evaluator : s.evaluators) {
    if (!evaluator.Run(batch, &s.status)) {
      return false;
    }
  }
  // Added only once all have run: each reads the batch as it came.
  for (auto& evaluator : s.evaluators) {
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

}  // namespace perfetto::trace_processor::core::exec
