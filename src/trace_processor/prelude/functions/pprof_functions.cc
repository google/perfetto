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
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/profile_builder.h"
#include "src/trace_processor/util/status_macros.h"

#include <cinttypes>
#include <cstdint>
#include <limits>
#include <utility>

// TODO(carlscab): We currently recreate the GProfileBuilder for every group. We
// should cache this somewhere maybe even have a helper table that stores all
// this data.

namespace perfetto {
namespace trace_processor {
namespace {

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

class ProfileFunctionBase {
 public:
  static base::Status Register(sqlite3* db,
                               std::unique_ptr<ProfileFunctionBase> function) {
    int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC;
    // Keep a copy of the name. If registration fails function will be deleted
    // and thus the name will no longer be available for the error message.
    std::string function_name = function->name();
    int n_arg = function->GetArgumentCount();
    int ret = sqlite3_create_function_v2(db, function_name.c_str(), n_arg,
                                         flags, function.release(), nullptr,
                                         Step, Final, Destroy);
    if (ret != SQLITE_OK) {
      return base::ErrStatus("Unable to register function with name %s",
                             function_name.c_str());
    }
    return base::OkStatus();
  }

  virtual ~ProfileFunctionBase() {}

 protected:
  ProfileFunctionBase(const TraceProcessorContext* tp_context,
                      std::string name,
                      bool annotate_callsites)
      : tp_context_(tp_context),
        name_(std::move(name)),
        annotate_callsites_(annotate_callsites) {}

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
    ProfileFunctionBase* func =
        reinterpret_cast<ProfileFunctionBase*>(sqlite3_user_data(ctx));

    base::Status status = func->StepImpl(ctx, argc, argv);

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
    delete reinterpret_cast<ProfileFunctionBase*>(p_app);
  }

  static base::Status GetCallsiteId(sqlite3_value* arg, uint32_t& callsite_id) {
    int64_t value = sqlite3_value_int64(arg);

    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) {
      return base::ErrStatus("invalid callsite_id value %" PRId64, value);
    }

    callsite_id = static_cast<uint32_t>(value);
    return util::OkStatus();
  }

  virtual base::Status ValidateArguments(int argc,
                                         sqlite3_value** argv) const = 0;
  // Will only be called if ValidateArguments returned successfully.
  virtual base::Status GetSampleTypes(
      int argc,
      sqlite3_value** argv,
      std::vector<GProfileBuilder::ValueType>& sample_types) const = 0;

  // Will only be called if ValidateArguments returned successfully.
  virtual base::Status AddSample(uint32_t callsite_id,
                                 int argc,
                                 sqlite3_value** argv,
                                 GProfileBuilder& builder) = 0;

  // Returns number of arguments expected by the function. -1 for variable
  // number of arguments.
  virtual int GetArgumentCount() const = 0;

  const std::string& name() const { return name_; }

  base::Status StepImpl(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    GProfileBuilder** builder = reinterpret_cast<GProfileBuilder**>(
        sqlite3_aggregate_context(ctx, sizeof(GProfileBuilder*)));
    if (!builder) {
      return base::ErrStatus("Failed to allocate aggregate context");
    }

    if (!*builder) {
      RETURN_IF_ERROR(ValidateArguments(argc, argv));
      RETURN_IF_ERROR(ValidateCallsiteIdArgument(argc, argv));
      RETURN_IF_ERROR(CreateProfileBuilder(argc, argv, builder));
    }

    // Needs to be initialized to avoid compiler warning. Do it to a most likely
    // invalid callsite_id.
    uint32_t callsite_id = std::numeric_limits<uint32_t>::max();
    RETURN_IF_ERROR(GetCallsiteId(argv[0], callsite_id));

    return AddSample(callsite_id, argc, argv, **builder);
  }

  base::Status CreateProfileBuilder(int argc,
                                    sqlite3_value** argv,
                                    GProfileBuilder** builder) {
    std::vector<GProfileBuilder::ValueType> sample_types;
    RETURN_IF_ERROR(GetSampleTypes(argc, argv, sample_types));
    *builder = new GProfileBuilder(tp_context_, std::move(sample_types),
                                   annotate_callsites_);
    return util::OkStatus();
  }

  base::Status ValidateCallsiteIdArgument(int argc,
                                          sqlite3_value** argv) const {
    if (argc < 1) {
      return base::ErrStatus("missing argument callstack_id");
    }

    base::Status status =
        sqlite_utils::TypeCheckSqliteValue(argv[0], SqlValue::kLong);
    if (!status.ok()) {
      return base::ErrStatus("argument 1; value %s", status.c_message());
    }

    return util::OkStatus();
  }

  const TraceProcessorContext* const tp_context_;
  const std::string name_;
  const bool annotate_callsites_;
};

class PerfProfileFunction : public ProfileFunctionBase {
 public:
  PerfProfileFunction(const TraceProcessorContext* context,
                      std::string name,
                      bool annotate_callsites)
      : ProfileFunctionBase(context, name, annotate_callsites) {
    single_count_value_.Append(1);
  }

 private:
  int GetArgumentCount() const override { return 1; }

  base::Status ValidateArguments(int argc, sqlite3_value**) const override {
    if (GetArgumentCount() != argc) {
      return base::ErrStatus(
          "invalid number of args; "
          "expected %d, received %d",
          GetArgumentCount(), argc);
    }
    return util::OkStatus();
  }

  base::Status GetSampleTypes(
      int,
      sqlite3_value**,
      std::vector<GProfileBuilder::ValueType>& sample_types) const override {
    sample_types = {{"samples", "count"}};
    return util::OkStatus();
  }

  base::Status AddSample(uint32_t callsite_id,
                         int,
                         sqlite3_value**,
                         GProfileBuilder& builder) override {
    if (!builder.AddSample(callsite_id, single_count_value_)) {
      return base::ErrStatus("invalid callsite_id: %" PRIu32, callsite_id);
    }
    return util::OkStatus();
  }

  protozero::PackedVarInt single_count_value_;
};

class ProfileFunction : public ProfileFunctionBase {
 public:
  ProfileFunction(const TraceProcessorContext* context,
                  std::string name,
                  bool annotate_callsites)
      : ProfileFunctionBase(context, name, annotate_callsites) {}

 private:
  int GetArgumentCount() const override { return -1; }

  base::Status GetSampleTypes(
      int argc,
      sqlite3_value** argv,
      std::vector<GProfileBuilder::ValueType>& sample_types) const override {
    std::vector<GProfileBuilder::ValueType> tmp;

    PERFETTO_CHECK(argc > 1 && (argc - 1) % 3 == 0);

    for (int i = 1; i < argc; i += 3) {
      GProfileBuilder::ValueType value_type;
      value_type.type =
          reinterpret_cast<const char*>(sqlite3_value_text(argv[i]));
      value_type.unit =
          reinterpret_cast<const char*>(sqlite3_value_text(argv[i + 1]));
      tmp.push_back(std::move(value_type));
    }

    sample_types = std::move(tmp);
    return util::OkStatus();
  }

  base::Status ValidateArguments(int argc,
                                 sqlite3_value** argv) const override {
    if (argc == 0) {
      return base::ErrStatus(
          "arguments missing; expected callsite_id, type, unit, and value");
    }
    if (argc == 1) {
      return base::ErrStatus(
          "arguments missing; expected type, unit, and value");
    }

    for (int i = 1; i < argc;) {
      base::Status status =
          sqlite_utils::TypeCheckSqliteValue(argv[i], SqlValue::kString);
      if (!status.ok()) {
        return base::ErrStatus("argument %d; type %s", i + 1,
                               status.c_message());
      }
      ++i;
      if (i == argc) {
        return base::ErrStatus("arguments missing; expected unit, value");
      }
      status = sqlite_utils::TypeCheckSqliteValue(argv[i], SqlValue::kString);
      if (!status.ok()) {
        return base::ErrStatus("argument %d; unit %s", i + 1,
                               status.c_message());
      }
      ++i;
      if (i == argc) {
        return base::ErrStatus("argument missing; expected value");
      }
      status = sqlite_utils::TypeCheckSqliteValue(argv[i], SqlValue::kLong);
      if (!status.ok()) {
        return base::ErrStatus("argument %d; value %s", i + 1,
                               status.c_message());
      }
      ++i;
    }
    return util::OkStatus();
  }

  base::Status AddSample(uint32_t callsite_id,
                         int argc,
                         sqlite3_value** argv,
                         GProfileBuilder& builder) override {
    PERFETTO_CHECK(argc >= 4 && (argc - 1) % 3 == 0);

    protozero::PackedVarInt values;

    for (int i = 3; i < argc; i += 3) {
      values.Append(sqlite3_value_int64(argv[i]));
    }

    if (!builder.AddSample(callsite_id, values)) {
      return base::ErrStatus("invalid callsite_id: %" PRIu32, callsite_id);
    }
    return util::OkStatus();
  }
};

}  // namespace

base::Status PprofFunctions::Register(sqlite3* db,
                                      TraceProcessorContext* context) {
  RETURN_IF_ERROR(PerfProfileFunction::Register(
      db, MakeUnique<PerfProfileFunction>(
              context, "EXPERIMENTAL_ANNOTATED_PERF_PROFILE", true)));
  RETURN_IF_ERROR(PerfProfileFunction::Register(
      db, MakeUnique<PerfProfileFunction>(context, "EXPERIMENTAL_PERF_PROFILE",
                                          false)));

  RETURN_IF_ERROR(ProfileFunction::Register(
      db, MakeUnique<ProfileFunction>(context, "EXPERIMENTAL_ANNOTATED_PROFILE",
                                      true)));
  RETURN_IF_ERROR(ProfileFunction::Register(
      db, MakeUnique<ProfileFunction>(context, "EXPERIMENTAL_PROFILE", false)));

  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
