/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TABLES_MACROS_H_
#define SRC_TRACE_PROCESSOR_TABLES_MACROS_H_

#include "src/trace_processor/tables/macros_internal.h"

namespace perfetto {
namespace trace_processor {

// Usage of the below macros
// These macros have two different invocation patterns depending on whether you
// are defining a root table or a derived table (see below for definitions and
// examples). If you're not sure which one you need, you probably want a derived
// table.
//
// Root tables
// Root tables act as the ultimate parent of a heirarcy of tables. All rows of
// child tables will be some subset of rows in the parent. Real world examples
// of root tables include EventTable and TrackTable.
//
// All root tables implicitly contain an 'id' column which contains the row
// index for each row in the table.
//
// Suppose we want to define EventTable with columns 'ts' and 'arg_set_id'.
//
// Then we would invoke the macro as follows:
// #define PERFETTO_TP_EVENT_TABLE_DEF(NAME, PARENT, C)
//   NAME(EventTable)
//   PARENT(PERFETTO_TP_ROOT_TABLE_PARENT_DEF, C)
//   C(int64_t, ts)
//   C(uint32_t, arg_set_id)
// PERFETTO_TP_TABLE(PERFETTO_TP_EVENT_TABLE_DEF);
//
// Note that PERFETTO_TP_ROOT_TABLE_PARENT_DEF is passed as the parent
// definition; this is a builtin macro (defined below) that acts as a parent
// for all root tables.
//
// Derived tables
// Suppose we want to derive a table called SliceTable which inherits all
// columns from EventTable (with EventTable's definition macro being
// PERFETTO_TP_EVENT_TABLE_DEF) and columns 'dur' and 'depth'.
//
// Then, we would invoke the macro as follows:
// #define PERFETTO_TP_SLICE_TABLE_DEF(NAME, PARENT, C)
//   NAME(ChildTable)
//   PARENT(PERFETTO_TP_EVENT_TABLE_DEF, C)
//   C(int64_t, dur)
//   C(uint8_t, depth)
// PERFETTO_TP_TABLE(PERFETTO_TP_SLICE_TABLE_DEF);

// Macro definition using when defining a new root table.
//
// This macro can be passed to PARENT as the parent definition for root tables.
// This allows for correct type-checking of root tables.
//
// See the top of the file for how this should be used.
#define PERFETTO_TP_ROOT_TABLE_PARENT_DEF(NAME, PARENT, C) \
  NAME(macros_internal::RootParentTable)

// The macro used to define storage backed tables.
// See the top of the file for how this should be used.
//
// This macro takes one argument: the full definition of the table; the
// definition is a function macro taking three arguments:
// 1. NAME, a function macro taking one argument: the name of the new class
//    being defined.
// 2. PARENT, a function macro taking two arguments: a) the definition of
//    the parent table (or PERFETTO_TP_ROOT_TABLE_PARENT_DEF) if this table
//    is a root table b) C, the third parameter of the macro definition (see
//    below).
// 3. C, a function macro taking two parameters: a) the type of a column
//    b) the name of a column. This macro should be invoked as many times as
//    there are columns in the table with the information about them.
#define PERFETTO_TP_TABLE(DEF)      \
  PERFETTO_TP_TABLE_INTERNAL(       \
      PERFETTO_TP_TABLE_CLASS(DEF), \
      PERFETTO_TP_TABLE_CLASS(PERFETTO_TP_PARENT_DEF(DEF)), DEF)

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_MACROS_H_
