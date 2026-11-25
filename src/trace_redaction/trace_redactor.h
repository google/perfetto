/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_TRACE_REDACTOR_H_
#define SRC_TRACE_REDACTION_TRACE_REDACTOR_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Represents a single redaction pass containing its own sequence of collect,
// build, transform, and augment primitives.
class ConcreteRedactorPass {
 public:
  ConcreteRedactorPass();
  virtual ~ConcreteRedactorPass();

  // T must be derived from trace_redaction::CollectPrimitive.
  template <typename T>
  T* emplace_collect() {
    auto uptr = std::make_unique<T>();
    auto* ptr = uptr.get();
    collectors_.push_back(std::move(uptr));
    return ptr;
  }

  // T must be derived from trace_redaction::ValidatorPrimitive.
  template <typename T>
  T* emplace_validator() {
    auto uptr = std::make_unique<T>();
    auto* ptr = uptr.get();
    validators_.push_back(std::move(uptr));
    return ptr;
  }

  // T must be derived from trace_redaction::BuildPrimitive.
  template <typename T>
  T* emplace_build() {
    auto uptr = std::make_unique<T>();
    auto* ptr = uptr.get();
    builders_.push_back(std::move(uptr));
    return ptr;
  }

  // T must be derived from trace_redaction::TransformPrimitive.
  template <typename T>
  T* emplace_transform() {
    auto uptr = std::make_unique<T>();
    auto* ptr = uptr.get();
    transformers_.push_back(std::move(uptr));
    return ptr;
  }

  // T must be derived from trace_redaction::AugmentPrimitive.
  template <typename T>
  T* emplace_augment() {
    auto uptr = std::make_unique<T>();
    auto* ptr = uptr.get();
    augmenters_.push_back(std::move(uptr));
    return ptr;
  }

  // Returns true when no primitives have been registered on this pass.
  bool empty() const {
    return collectors_.empty() && validators_.empty() && builders_.empty() &&
           transformers_.empty() && augmenters_.empty();
  }

  // When true, Collect fails if the trace buffer has trailing bytes that do
  // not decode as packets (i.e. the trace is truncated or corrupt).
  void set_reject_trailing_bytes(bool reject) {
    reject_trailing_bytes_ = reject;
  }

  // Executes the pass: Collect -> Validate -> Build -> Transform -> Augment.
  // Transformed and augmented packets are appended to `output_buffer`.
  base::Status Redact(std::string_view view,
                      Context* context,
                      std::string* output_buffer) const;

 private:
  // Runs all collectors on a packet before moving to the next packet.
  // Collectors add low level information to the context.
  //
  // ```
  //  with context:
  //   for packet in packets:
  //     for collector in collectors:
  //       collector(context, packet)
  // ```
  base::Status Collect(Context* context, std::string_view view) const;

  // Runs all validators once on the context. Validators verify invariants
  // across the entire trace after all the data has been collected. If any
  // validator fails, the pass fails and the redactor bails out.
  //
  // ```
  //  with context:
  //   for validator in validators:
  //     validator(context)
  // ```
  base::Status Validate(const Context& context) const;

  // Runs builders once. Builders synthesize high-level information
  // based on low level data collected during `Collect`.
  //
  // ```
  //  with context:
  //   for builder in builders:
  //      builder(context)
  // ```
  base::Status Build(Context* context) const;

  // Runs all transformers on each packet, appending surviving packets to
  // `output_buffer`. Transformers can modify packets in place or drop them.
  //
  // ```
  //  with context:
  //   for packet in packets:
  //     for transform in transformers:
  //       transform(context, packet)
  // ```
  base::Status Transform(const Context& context,
                         std::string_view view,
                         std::string* output_buffer) const;

  // Runs all augmenters, appending generated packets to `output_buffer`
  // Augmenters inject new packets into the trace based on the context.
  //
  // ```
  //  with context:
  //    for augmenter in augmenters:
  //      augmenter(context, output_buffer)
  // ```
  base::Status Augment(const Context& context,
                       std::string* output_buffer) const;

  std::vector<std::unique_ptr<CollectPrimitive>> collectors_;
  std::vector<std::unique_ptr<ValidatorPrimitive>> validators_;
  std::vector<std::unique_ptr<BuildPrimitive>> builders_;
  std::vector<std::unique_ptr<TransformPrimitive>> transformers_;
  std::vector<std::unique_ptr<AugmentPrimitive>> augmenters_;

  bool reject_trailing_bytes_ = false;
};

// Orchestrates multi-pass trace redaction by executing one or more
// ConcreteRedactorPass instances sequentially.
class TraceRedactor {
 public:
  // Populates a pass dynamically from `Context` right before the pass runs,
  // after all earlier passes have completed. Leaving the pass empty skips it
  // with zero buffer allocation or copy.
  using PassBuilder =
      std::function<void(const Context&, ConcreteRedactorPass*)>;

  TraceRedactor();
  virtual ~TraceRedactor();

  // Adds a new pass to the redaction pipeline and returns a pointer to it.
  ConcreteRedactorPass* add_pass();

  // Adds a dynamic pass prepared from `Context` right before execution.
  void add_pass(PassBuilder builder);

  // Entry point for redacting a trace. Coordinates execution across all passes.
  // Regardless of success/failure, `context` will contain the current state.
  base::Status Redact(std::string_view source_filename,
                      std::string_view dest_filename,
                      Context* context) const;

  // In-memory variant of Redact(). `input` is a serialized, uncompressed
  // Trace and must stay valid for the duration of the call. `output` is
  // cleared and receives the redacted serialized Trace.
  base::Status RedactBuffer(std::string_view input,
                            std::string* output,
                            Context* context) const;

  // Convenience helper methods that forward to the last concrete pass
  // (creates a pass if needed).
  template <typename T>
  T* emplace_collect() {
    return GetLastConcretePass()->emplace_collect<T>();
  }

  template <typename T>
  T* emplace_validator() {
    return GetLastConcretePass()->emplace_validator<T>();
  }

  template <typename T>
  T* emplace_build() {
    return GetLastConcretePass()->emplace_build<T>();
  }

  template <typename T>
  T* emplace_transform() {
    return GetLastConcretePass()->emplace_transform<T>();
  }

  template <typename T>
  T* emplace_augment() {
    return GetLastConcretePass()->emplace_augment<T>();
  }

  struct Config {
    // Controls whether or not the verify primitive is added to the pipeline.
    // This should always be enabled unless you know that your test content
    // fails verification.
    bool verify = true;
  };

  // Creates the unified trace redactor handling system traces, Java heap dump
  // traces, and combined traces.
  static std::unique_ptr<TraceRedactor> CreateInstance(const Config& config);

 private:
  struct RedactorPass {
    std::unique_ptr<ConcreteRedactorPass> concrete_pass;
    PassBuilder builder;
  };

  ConcreteRedactorPass* GetLastConcretePass() {
    if (passes_.empty() || !passes_.back().concrete_pass) {
      return add_pass();
    }
    return passes_.back().concrete_pass.get();
  }

  std::vector<RedactorPass> passes_;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_TRACE_REDACTOR_H_
