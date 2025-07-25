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

#ifndef SRC_TRACED_PROBES_FTRACE_CPU_STATS_PARSER_H_
#define SRC_TRACED_PROBES_FTRACE_CPU_STATS_PARSER_H_

#include <string>

namespace perfetto {

class Tracefs;
struct FtraceStats;
struct FtraceCpuStats;

bool DumpCpuStats(std::string text, FtraceCpuStats*);
bool DumpAllCpuStats(Tracefs*, FtraceStats*);

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_CPU_STATS_PARSER_H_
