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

#include "src/trace_processor/sqlite/pprof_function.h"

#include "perfetto/trace_processor/status.h"
#include "src/profiling/profile_builder.h"
#include "src/trace_processor/sqlite/create_function_internal.h"

// TODO(carlscab): We currently recreate the GProfileBuilder for every group. We
// should cache this somewhere maybe even have a helper table that stores all
// this data.

namespace perfetto {
namespace trace_processor {
namespace {

void SetSqliteError(sqlite3_context* ctx, const base::Status& status) {
  if (!status.ok()) {
    sqlite3_result_error(ctx, status.c_message(), -1);
  }
}

class Profile {
 public:
  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv);
  static void Final(sqlite3_context* ctx);

 private:
  static std::unique_ptr<Profile> Release(sqlite3_context* ctx) {
    Profile** profile =
        reinterpret_cast<Profile**>(sqlite3_aggregate_context(ctx, 0));

    if (!profile) {
      return nullptr;
    }

    return std::unique_ptr<Profile>(*profile);
  }

  static Profile* GetOrCreate(sqlite3_context* ctx, bool annotate_frames) {
    Profile** profile = reinterpret_cast<Profile**>(
        sqlite3_aggregate_context(ctx, sizeof(Profile*)));
    if (!profile) {
      return nullptr;
    }

    if (!*profile) {
      *profile =
          new Profile(reinterpret_cast<TraceProcessor*>(sqlite3_user_data(ctx)),
                      annotate_frames);
    }

    return *profile;
  }

  Profile(TraceProcessor* tp, bool annotate_frames);

  base::Status StepImpl(int64_t callsite_id, bool annotate_frames);
  base::Status FinalImpl(sqlite3_context*);

  const bool annotate_frames_;
  ::perfetto::profiling::GProfileBuilder builder_;
  protozero::PackedVarInt single_count_value_;
};

Profile::Profile(TraceProcessor* tp, bool annotate_frames)
    : annotate_frames_(annotate_frames), builder_(tp, annotate_frames) {
  single_count_value_.Append(1);
  builder_.WriteSampleTypes({{"samples", "count"}});
}

void Profile::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != 1 && argc != 2) {
    return SetSqliteError(
        ctx, base::ErrStatus("EXPERIMENTAL_PPROF: invalid number of args; "
                             "expected 1 or 2, received %d",
                             argc));
  }

  base::Status status = TypeCheckSqliteValue(argv[0], SqlValue::kLong);
  if (!status.ok()) {
    return SetSqliteError(
        ctx, base::ErrStatus("EXPERIMENTAL_PPROF: argument callsite_id %s",
                             status.c_message()));
  }
  int64_t callsite_id = sqlite3_value_int64(argv[0]);

  bool annotate_frames = true;

  if (argc == 2) {
    status = TypeCheckSqliteValue(argv[1], SqlValue::kLong);
    if (!status.ok()) {
      return SetSqliteError(
          ctx,
          base::ErrStatus("EXPERIMENTAL_PPROF: argument annotate_frames %s",
                          status.c_message()));
    }
    annotate_frames = sqlite3_value_int64(argv[1]) != 0;
  }

  Profile* profile = Profile::GetOrCreate(ctx, annotate_frames);

  if (!profile) {
    return SetSqliteError(
        ctx, base::ErrStatus(
                 "EXPERIMENTAL_PPROF: Failed to allocate aggregate context"));
  }

  status = profile->StepImpl(callsite_id, annotate_frames);

  if (!status.ok()) {
    return SetSqliteError(ctx, status);
  }
}

void Profile::Final(sqlite3_context* ctx) {
  std::unique_ptr<Profile> profile = Profile::Release(ctx);
  if (!profile) {
    return;
  }

  base::Status status = profile->FinalImpl(ctx);
  if (!status.ok()) {
    return SetSqliteError(ctx, status);
  }
}

base::Status Profile::StepImpl(int64_t callsite_id, bool annotate_frames) {
  if (annotate_frames_ != annotate_frames) {
    return base::ErrStatus(
        "EXPERIMENTAL_PPROF: argument annotate_frames must be constant");
  }

  builder_.AddSample(single_count_value_, callsite_id);

  return util::OkStatus();
}

base::Status Profile::FinalImpl(sqlite3_context* ctx) {
  // TODO: A lot of copies are happening here.
  std::string profile_proto = builder_.CompleteProfile();
  std::unique_ptr<uint8_t[], base::FreeDeleter> data(
      static_cast<uint8_t*>(malloc(profile_proto.size())));
  memcpy(data.get(), profile_proto.data(), profile_proto.size());
  sqlite3_result_blob(ctx, data.release(),
                      static_cast<int>(profile_proto.size()), free);
  return util::OkStatus();
}

}  // namespace

base::Status PprofFunction::Register(sqlite3* db, TraceProcessor* tp) {
  int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC;
  int ret = sqlite3_create_function_v2(db, "EXPERIMENTAL_PPROF", -1, flags, tp,
                                       nullptr, Profile::Step, Profile::Final,
                                       nullptr);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "Unable to register function with name EXPERIMENTAL_PPROF");
  }
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
