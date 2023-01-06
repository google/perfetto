/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_REGISTER_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_REGISTER_FUNCTION_H_

#include <sqlite3.h>
#include <memory>

#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

// Prototype for a C++ function which can be registered with SQLite.
//
// Usage
//
// Define a subclass of this struct as follows:
// struct YourFunction : public SqlFunction {
//   // Optional if you want a custom context object (i.e. an object
//   // passed in at registration time which will be passed to Run on
//   // every invocation)
//   struct YourContext { /* define context fields here */ };
//
//   static base::Status Run(/* see parameters below */) {
//     /* function body here */
//   }
//
//   static base::Status Cleanup(/* see parameters below */) {
//     /* function body here */
//   }
// }
//
// Then, register this function with SQLite using RegisterFunction (see below);
// you'll likely want to do this in TraceProcessorImpl:
// RegisterFunction<YourFunction>(/* see arguments below */)
struct SqlFunction {
  // The type of the context object which will be passed to the function.
  // Can be redefined in any sub-classes to override the context.
  using Context = void;

  // Indicates whether this function is "void" (i.e. doesn't actually want
  // to return a value). While the function will still return null in SQL
  // (because SQLite does not actually allow null functions), for accounting
  // purposes, this null will be ignored when verifying whether this statement
  // has any output.
  // Can be redefined in any sub-classes to override it.
  // If this is set to true, subclasses must not modify |out| or |destructors|.
  static constexpr bool kVoidReturn = false;

  // Struct which holds destructors for strings/bytes returned from the
  // function. Passed as an argument to |Run| to allow implementations to
  // override the destructors.
  struct Destructors {
    sqlite3_destructor_type string_destructor = sqlite_utils::kSqliteTransient;
    sqlite3_destructor_type bytes_destructor = sqlite_utils::kSqliteTransient;
  };

  // The function which will be exectued with the arguments from SQL.
  //
  // Implementations MUST define this function themselves; this function is
  // declared but *not* defined so linker errors will be thrown if not defined.
  //
  // |ctx|:         the context object passed at registration time.
  // |argc|:        number of arguments.
  // |argv|:        arguments to the function.
  // |out|:         the return value of the function.
  // |destructors|: destructors for string/bytes return values.
  static base::Status Run(Context* ctx,
                          size_t argc,
                          sqlite3_value** argv,
                          SqlValue& out,
                          Destructors& destructors);

  // Executed after the result from |Run| is reported to SQLite.
  // Allows implementations to verify post-conditions without needing to worry
  // about overwriting return types.
  //
  // Implementations do not need to define this function; a default no-op
  // implementation will be used in this case.
  static base::Status VerifyPostConditions(Context*);

  // Executed after the result from |Run| is reported to SQLite.
  // Allows any pending state to be cleaned up post-copy of results by SQLite:
  // this function will be called even if |Run| or |PostRun| returned errors.
  //
  // Implementations do not need to define this function; a default no-op
  // implementation will be used in this case.
  static void Cleanup(Context*);
};

// Registers a C++ function to be runnable from SQL.
// The format of the function is given by the |SqlFunction|; see the
// documentaion above.
//
// |db|:          sqlite3 database object
// |name|:        name of the function in SQL
// |argc|:        number of arguments for this function, -1 if variable
// |ctx|:         context object for the function (see SqlFunction::Run above);
//                this object *must* outlive the function so should likely be
//                either static or scoped to the lifetime of TraceProcessor.
// |determistic|: whether this function has deterministic output given the
//                same set of arguments.
template <typename Function>
base::Status RegisterSqlFunction(sqlite3* db,
                                 const char* name,
                                 int argc,
                                 typename Function::Context* ctx,
                                 bool deterministic = true);

// Same as above except allows a unique_ptr to be passed for the context; this
// allows for SQLite to manage the lifetime of this pointer instead of the
// essentially static requirement of the context pointer above.
template <typename Function>
base::Status RegisterSqlFunction(
    sqlite3* db,
    const char* name,
    int argc,
    std::unique_ptr<typename Function::Context> ctx,
    bool deterministic = true);

}  // namespace trace_processor
}  // namespace perfetto

// The rest of this file is just implementation details which we need
// in the header file because it is templated code. We separate it out
// like this to keep the API people actually care about easy to read.

namespace perfetto {
namespace trace_processor {

namespace sqlite_internal {

// RAII type to call Function::Cleanup when destroyed.
template <typename Function>
struct ScopedCleanup {
  typename Function::Context* ctx;
  ~ScopedCleanup() { Function::Cleanup(ctx); }
};

template <typename Function>
void WrapSqlFunction(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  using Context = typename Function::Context;
  Context* ud = static_cast<Context*>(sqlite3_user_data(ctx));

  ScopedCleanup<Function> scoped_cleanup{ud};
  SqlValue value{};
  SqlFunction::Destructors destructors{};
  base::Status status =
      Function::Run(ud, static_cast<size_t>(argc), argv, value, destructors);
  if (!status.ok()) {
    sqlite3_result_error(ctx, status.c_message(), -1);
    return;
  }

  if (Function::kVoidReturn) {
    if (!value.is_null()) {
      sqlite3_result_error(ctx, "void SQL function returned value", -1);
      return;
    }

    // If the function doesn't want to return anything, set the "VOID"
    // pointer type to a non-null value. Note that because of the weird
    // way |sqlite3_value_pointer| works, we need to set some value even
    // if we don't actually read it - just set it to a pointer to an empty
    // string for this reason.
    static char kVoidValue[] = "";
    sqlite3_result_pointer(ctx, kVoidValue, "VOID", nullptr);
  } else {
    sqlite_utils::ReportSqlValue(ctx, value, destructors.string_destructor,
                                 destructors.bytes_destructor);
  }

  status = Function::VerifyPostConditions(ud);
  if (!status.ok()) {
    sqlite3_result_error(ctx, status.c_message(), -1);
    return;
  }
}
}  // namespace sqlite_internal

template <typename Function>
base::Status RegisterSqlFunction(sqlite3* db,
                                 const char* name,
                                 int argc,
                                 typename Function::Context* ctx,
                                 bool deterministic) {
  int flags = SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0);
  int ret = sqlite3_create_function_v2(
      db, name, static_cast<int>(argc), flags, ctx,
      sqlite_internal::WrapSqlFunction<Function>, nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    return base::ErrStatus("Unable to register function with name %s", name);
  }
  return base::OkStatus();
}

template <typename Function>
base::Status RegisterSqlFunction(
    sqlite3* db,
    const char* name,
    int argc,
    std::unique_ptr<typename Function::Context> user_data,
    bool deterministic) {
  int flags = SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0);
  int ret = sqlite3_create_function_v2(
      db, name, static_cast<int>(argc), flags, user_data.release(),
      sqlite_internal::WrapSqlFunction<Function>, nullptr, nullptr,
      [](void* ptr) { delete static_cast<typename Function::Context*>(ptr); });
  if (ret != SQLITE_OK) {
    return base::ErrStatus("Unable to register function with name %s", name);
  }
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_REGISTER_FUNCTION_H_
