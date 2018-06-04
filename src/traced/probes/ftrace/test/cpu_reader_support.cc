/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/traced/probes/ftrace/test/cpu_reader_support.h"

#include "perfetto/base/utils.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"

#include <string.h>

namespace perfetto {
namespace {

std::map<std::string, std::unique_ptr<ProtoTranslationTable>>* g_tables;

}  // namespace

ProtoTranslationTable* GetTable(const std::string& name) {
  if (!g_tables)
    g_tables =
        new std::map<std::string, std::unique_ptr<ProtoTranslationTable>>();
  if (!g_tables->count(name)) {
    std::string path = "src/traced/probes/ftrace/test/data/" + name + "/";
    FtraceProcfs ftrace(path);
    auto table = ProtoTranslationTable::Create(&ftrace, GetStaticEventInfo(),
                                               GetStaticCommonFieldsInfo());
    g_tables->emplace(name, std::move(table));
  }
  return g_tables->at(name).get();
}

std::unique_ptr<uint8_t[]> PageFromXxd(const std::string& text) {
  auto buffer = std::unique_ptr<uint8_t[]>(new uint8_t[base::kPageSize]);
  const char* ptr = text.data();
  memset(buffer.get(), 0xfa, base::kPageSize);
  uint8_t* out = buffer.get();
  while (*ptr != '\0') {
    if (*(ptr++) != ':')
      continue;
    for (int i = 0; i < 8; i++) {
      PERFETTO_CHECK(text.size() >=
                     static_cast<size_t>((ptr - text.data()) + 5));
      PERFETTO_CHECK(*(ptr++) == ' ');
      int n = sscanf(ptr, "%02hhx%02hhx", out, out + 1);
      PERFETTO_CHECK(n == 2);
      out += n;
      ptr += 4;
    }
    while (*ptr != '\n')
      ptr++;
  }
  return buffer;
}

}  // namespace perfetto
