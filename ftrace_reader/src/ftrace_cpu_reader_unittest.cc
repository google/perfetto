/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "ftrace_reader/ftrace_cpu_reader.h"
#include "ftrace_to_proto_translation_table.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace {

TEST(FtraceCpuReader, ParseEmpty) {
  std::string path = "ftrace_reader/test/data/android_seed_N2F62_3.10.49/";
  auto table = FtraceToProtoTranslationTable::Create(path);
  FtraceCpuReader(table.get(), 42, base::ScopedFile());
}

}  // namespace
}  // namespace perfetto
