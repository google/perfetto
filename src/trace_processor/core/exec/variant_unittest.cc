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

#include "src/trace_processor/core/exec/variant.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// A default-constructed Variant is deliberately uninitialized (see the header)
// so every value has to come from a factory, which carries its type.
TEST(VariantTest, FactoriesCarryTheirType) {
  EXPECT_EQ(Variant::Null().type, Variant::Type::kNull);
  EXPECT_EQ(Variant::Int64(7).AsInt64(), 7);
  EXPECT_EQ(Variant::Double(1.5).AsDouble(), 1.5);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
