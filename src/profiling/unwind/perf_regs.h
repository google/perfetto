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

#ifndef SRC_PROFILING_UNWIND_PERF_REGS_H_
#define SRC_PROFILING_UNWIND_PERF_REGS_H_

// kernel uapi headers for perf_regs
#include <uapi/asm-arm/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#include <uapi/asm-x86/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#define perf_event_arm_regs perf_event_arm64_regs
#include <uapi/asm-arm64/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#undef perf_event_arm_regs
#include <uapi/asm-riscv/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK

#endif  // SRC_PROFILING_UNWIND_PERF_REGS_H_
