/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/prelude/functions/pprof_functions.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/status.h"
#include "protos/perfetto/trace_processor/stack.pbzero.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/profile_builder.h"
#include "src/trace_processor/util/status_macros.h"

#include <malloc.h>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// TODO(carlscab): We currently recreate the GProfileBuilder for every group. We
// should cache this somewhere maybe even have a helper table that stores all
// this data.

namespace perfetto {
namespace trace_processor {
namespace {

using protos::pbzero::Stack;

template <typename T, typename... Args>
std::unique_ptr<T> MakeUnique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

void SetSqliteError(sqlite3_context* ctx, const base::Status& status) {
  PERFETTO_CHECK(!status.ok());
  sqlite3_result_error(ctx, status.c_message(), -1);
}

void SetSqliteError(sqlite3_context* ctx,
                    const std::string& function_name,
                    const base::Status& status) {
  SetSqliteError(ctx, base::ErrStatus("%s: %s", function_name.c_str(),
                                      status.c_message()));
}

// EXPERIMENTAL_[ANNOTATED_]PROFILE(
//      stack BLOB<Stack>, [type STRING, units STRING, value LONG])
//
// Aggregate function to create profiles in pprof format.
// Each row into the aggregation will become one sample in the profile. If only
// one argument is provides samples will have the value 1 associated to them
// with a type of "sample" and units "count". Alternatively you can specify the
// different values with each stack. If you do so you must also specify the type
// and the units for each of them.
//
// Note that type and units must be constants, undefined behaviour will result
// otherwise.
class ProfileFunction {
 public:
  static base::Status Register(sqlite3* db,
                               std::unique_ptr<ProfileFunction> function) {
    int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC;
    // Keep a copy of the name. If registration fails function will be deleted
    // and thus the name will no longer be available for the error message.
    std::string function_name = function->name();
    int ret = sqlite3_create_function_v2(db, function_name.c_str(), -1, flags,
                                         function.release(), nullptr, Step,
                                         Final, Destroy);
    if (ret != SQLITE_OK) {
      return base::ErrStatus("Unable to register function with name %s",
                             function_name.c_str());
    }
    return base::OkStatus();
  }

  ~ProfileFunction() = default;

  ProfileFunction(const TraceProcessorContext* tp_context,
                  std::string name,
                  bool annotate_callsites)
      : tp_context_(tp_context),
        name_(std::move(name)),
        annotate_callsites_(annotate_callsites) {
    sample_value_.Append(1);
  }

 private:
  static std::unique_ptr<GProfileBuilder> ReleaseProfileBuilder(
      sqlite3_context* ctx) {
    GProfileBuilder** builder =
        reinterpret_cast<GProfileBuilder**>(sqlite3_aggregate_context(ctx, 0));

    if (!builder) {
      return nullptr;
    }

    return std::unique_ptr<GProfileBuilder>(*builder);
  }

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_CHECK(argc >= 0);
    ProfileFunction* func =
        reinterpret_cast<ProfileFunction*>(sqlite3_user_data(ctx));

    base::Status status = func->StepImpl(ctx, static_cast<size_t>(argc), argv);

    if (!status.ok()) {
      SetSqliteError(ctx, func->name(), status);
    }
  }

  static void Final(sqlite3_context* ctx) {
    std::unique_ptr<GProfileBuilder> builder = ReleaseProfileBuilder(ctx);
    if (!builder) {
      return;
    }

    // TODO(carlscab): A lot of copies are happening here.
    std::string profile_proto = builder->Build();

    std::unique_ptr<uint8_t[], base::FreeDeleter> data(
        static_cast<uint8_t*>(malloc(profile_proto.size())));
    memcpy(data.get(), profile_proto.data(), profile_proto.size());
    sqlite3_result_blob(ctx, data.release(),
                        static_cast<int>(profile_proto.size()), free);
  }

  static void Destroy(void* p_app) {
    delete static_cast<ProfileFunction*>(p_app);
  }

  base::StatusOr<std::vector<GProfileBuilder::ValueType>> GetSampleTypes(
      size_t argc,
      sqlite3_value** argv) {
    std::vector<GProfileBuilder::ValueType> sample_types;

    if (argc == 1) {
      sample_types.push_back({"samples", "count"});
    }

    for (size_t i = 1; i < argc; i += 3) {
      base::StatusOr<SqlValue> type = sqlite_utils::ExtractArgument(
          argc, argv, "sample_type", i, SqlValue::kString);
      if (!type.ok()) {
        return type.status();
      }

      base::StatusOr<SqlValue> units = sqlite_utils::ExtractArgument(
          argc, argv, "sample_units", i + 1, SqlValue::kString);
      if (!units.ok()) {
        return units.status();
      }

      sample_types.push_back({type->AsString(), units->AsString()});
    }
    return std::move(sample_types);
  }

  base::Status UpdateSampleValue(size_t argc, sqlite3_value** argv) {
    if (argc == 1) {
      return base::OkStatus();
    }

    sample_value_.Reset();
    for (size_t i = 3; i < argc; i += 3) {
      base::StatusOr<SqlValue> value = sqlite_utils::ExtractArgument(
          argc, argv, "sample_value", i, SqlValue::kLong);
      if (!value.ok()) {
        return value.status();
      }
      sample_value_.Append(value->AsLong());
    }

    return base::OkStatus();
  }

  const std::string& name() const { return name_; }

  base::Status StepImpl(sqlite3_context* ctx,
                        size_t argc,
                        sqlite3_value** argv) {
    GProfileBuilder** builder = reinterpret_cast<GProfileBuilder**>(
        sqlite3_aggregate_context(ctx, sizeof(GProfileBuilder*)));
    if (!builder) {
      return base::ErrStatus("Failed to allocate aggregate context");
    }

    if (!*builder) {
      base::StatusOr<std::vector<GProfileBuilder::ValueType>> sample_types =
          GetSampleTypes(argc, argv);
      if (!sample_types.ok()) {
        return sample_types.status();
      }
      *builder = new GProfileBuilder(
          tp_context_, std::move(sample_types.value()), annotate_callsites_);
    }

    RETURN_IF_ERROR(UpdateSampleValue(argc, argv));

    base::StatusOr<SqlValue> value =
        sqlite_utils::ExtractArgument(argc, argv, "stack", 0, SqlValue::kBytes);
    if (!value.ok()) {
      return value.status();
    }

    Stack::Decoder stack(static_cast<const uint8_t*>(value->bytes_value),
                         value->bytes_count);
    if (stack.bytes_left() != 0) {
      return sqlite_utils::ToInvalidArgumentError(
          "stack", 0, base::ErrStatus("failed to deserialize Stack proto"));
    }
    if (!(*builder)->AddSample(stack, sample_value_)) {
      return base::ErrStatus("Failed to add callstack");
    }
    return util::OkStatus();
  }

  const TraceProcessorContext* const tp_context_;
  const std::string name_;
  const bool annotate_callsites_;
  protozero::PackedVarInt sample_value_;
};

}  // namespace

base::Status PprofFunctions::Register(sqlite3* db,
                                      TraceProcessorContext* context) {
  RETURN_IF_ERROR(ProfileFunction::Register(
      db, MakeUnique<ProfileFunction>(context, "EXPERIMENTAL_ANNOTATED_PROFILE",
                                      true)));
  RETURN_IF_ERROR(ProfileFunction::Register(
      db, MakeUnique<ProfileFunction>(context, "EXPERIMENTAL_PROFILE", false)));

  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
