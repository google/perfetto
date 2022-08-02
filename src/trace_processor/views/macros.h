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

#ifndef SRC_TRACE_PROCESSOR_VIEWS_MACROS_H_
#define SRC_TRACE_PROCESSOR_VIEWS_MACROS_H_

#include "src/trace_processor/views/macros_internal.h"

namespace perfetto {
namespace trace_processor {

// The below macros allow defining C++ views with minimal boilerplate.
//
// Suppose you want to define a view which joins two tables slice and track.
// slice has columns: id, ts, dur and name, track_id
// track has columns: id, name
//
// If we were to define this view in SQL, it would look as follows:
// CREATE VIEW slice_with_track AS
// SELECT
//   slice.id AS id,
//   slice.ts AS ts,
//   slice.dur AS dur,
//   slice.name AS name,
//   slice.track_id AS track_id,
//   track.name AS track_name
// FROM slice
// JOIN track ON track.id = slice.track_id;
//
// The corresponding C++ macro invocation would be:
// #define PERFETTO_TP_SLICE_TRACK_VIEW_DEF(NAME, FROM, JOIN, COL, _)
//   NAME(SliceWithTrackView, "slice_with_track")
//   COL(id, slice, id)
//   COL(ts, slice, ts)
//   COL(dur, slice, dur)
//   COL(name, slice, name)
//   COL(track_id, slice, track_id)
//   COL(track_name, track, name)
//   FROM(SliceTable, slice)
//   JOIN(TrackTable, track, id, slice, track_id, View::kIdAlwaysPresent)
// PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_SLICE_TRACK_VIEW_DEF);
//
// And in a .cc file:
// PERFETTO_TP_DEFINE_VIEW(SliceWithTrackView);
//
// A shorter (and less error prone) version of the syntax, can be used if you
// want to expose all the columns from the slice table. This involves passing
// the table defintion macro for the slice table to
// PERFETTO_TP_VIEW_EXPORT_FROM_COLS along with the FCOL argument: #define
// PERFETTO_TP_SLICE_TRACK_VIEW_DEF(NAME, FROM, JOIN, COL, FCOL)
//   NAME(SliceWithTrackView, "slice_with_track")
//   PERFETTO_TP_VIEW_EXPORT_FROM_COLS(PERFETTO_TP_SLICE_TABLE_DEF, FCOL)
//   COL(track_name, track, name)
//   FROM(SliceTable, slice)
//   JOIN(TrackTable, track, id, slice, track_id, View::kIdAlwaysPresent)
// PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_SLICE_TRACK_VIEW_DEF);

// The macro used to define C++ views.
// See the top of the file for how this should be used.
//
// This macro takes one argument: the full definition of the table; the
// definition is a function macro taking four arguments:
// 1. NAME, a function macro taking two arguments: the name of the new class
//    being defined and the name of the table when exposed to SQLite.
// 2. FROM, a function macro taking 2 arguments:
//      a) the class name of the "root" table of this view
//      b) the name of this table for use in the JOIN and COL macros (see below)
// 3. JOIN, a function macro taking 6 arguments:
//      a) the class name of the table which will be joined into this view on
//         the "right" side of the join.
//      b) the unique name of this table for use in subsequent JOIN and COL
//      c) the name of the column from the "right" side which will be joined
//         with the "left" side column.
//      d) the name of a previously introduced table (in a previous FROM
//         or JOIN invocation) which will be the "left" side of the join
//      e) the name of the column from the "left" side which will be joined with
//         the "right" side column.
//      f) a bit-mased composed of bitwise OR-ed flags from View::Flag or
//         View::kNoFlag if no flags apply.
//    This macro should be invoked as many times as there are tables to be
//    joined into the view.
// 4. COL, a function macro taking two or three parameters:
//      a) the name of the column in the view
//      b) the name of the table this column is created from
//      c) the name of the column in the table this column is created from
//    This macro should be invoked as many times as there are columns in the
//    view.
// 5. FCOL, an opaque macros which should be passed to
//    PERFETTO_TP_VIEW_EXPORT_FROM_COLS if all the columns in the FROM table
//    should be exposed in this view; see above for how this call should look
//    like.
#define PERFETTO_TP_DECLARE_VIEW(DEF)                            \
  PERFETTO_TP_VIEW_INTERNAL(                                     \
      PERFETTO_TP_VIEW_NAME(DEF, PERFETTO_TP_VIEW_NAME_EXTRACT), \
      PERFETTO_TP_VIEW_NAME(DEF, PERFETTO_TP_VIEW_CLASS_EXTRACT), DEF)

// Macro used to automatically expose all the columns in the FROM table
// in a view.
// See the top of the file for how this should be used.
#define PERFETTO_TP_VIEW_EXPORT_FROM_COLS(DEF, FCOL) \
  FCOL(from_table::Id, id)                           \
  FCOL(StringPool::Id, type)                         \
  PERFETTO_TP_ALL_COLUMNS(DEF, FCOL)

// Macro used to define destructors for C++ views.
// See the top of the file for how this should be used.
//
// Should be invoked in a .cc file to prevent compiler errors about weak
// vtables.
#define PERFETTO_TP_DEFINE_VIEW(class_name) \
  class_name::~class_name() = default;      \
  class_name::QueryResult::~QueryResult() = default

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_VIEWS_MACROS_H_
