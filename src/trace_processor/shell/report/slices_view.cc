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

#include "src/trace_processor/shell/report/slices_view.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/trace_processor/report.pbzero.h"
#include "src/trace_processor/shell/report/report_sink.h"
#include "src/trace_processor/shell/report/scope.h"
#include "src/trace_processor/shell/report/view_common.h"

namespace perfetto::trace_processor::shell {
namespace {

using protos::pbzero::ReportPacket;
using protos::pbzero::SectionInfo;

std::string WhereClause(const Scope& scope) {
  // Exclude unfinished slices (dur < 0), which would skew duration ranking.
  std::string where = "dur >= 0";
  if (!scope.name_glob.empty())
    where += " AND name GLOB '" + EscapeSqlLiteral(scope.name_glob) + "'";
  return where;
}

}  // namespace

std::string SlicesTableView::Sql(const Scope& scope) const {
  return R"(SELECT name, count() AS cnt, sum(dur) AS total_dur,
    max(dur) AS max_dur,
    100.0 * sum(dur) / (SELECT end_ts - start_ts FROM trace_bounds) AS pct
  FROM slice WHERE )" +
         WhereClause(scope) + R"( GROUP BY name ORDER BY total_dur DESC
  LIMIT )" +
         std::to_string(scope.top);
}

base::Status SlicesTableView::Emit(TraceProcessor* tp,
                                   const Scope& scope,
                                   ReportSink* sink,
                                   bool omit_if_empty) const {
  int64_t total_names = 0;
  int64_t total_slices = 0;
  {
    std::string sql =
        "SELECT count(DISTINCT name) AS names, count() AS n FROM slice WHERE " +
        WhereClause(scope);
    auto it = tp->ExecuteQuery(sql);
    if (it.Next()) {
      total_names = AsI64(it.Get(0));
      total_slices = AsI64(it.Get(1));
    }
    RETURN_IF_ERROR(it.Status());
  }
  if (omit_if_empty && total_slices == 0)
    return base::OkStatus();

  {
    protozero::HeapBuffered<ReportPacket> packet;
    auto* s = packet->set_section_info();
    s->set_title("Slices");
    s->set_noun("slices");
    s->set_view("flat");
    s->set_total_rows(total_names);
    s->set_shown_rows(std::min(scope.top, total_names));
    s->set_total_items(total_slices);
    s->set_item_noun("slice");
    s->set_row_noun("slice names");
    SetColumns(s, {{"Name", SectionInfo::CF_STRING},
                   {"Count", SectionInfo::CF_COUNT},
                   {"Total dur", SectionInfo::CF_DURATION},
                   {"% of trace", SectionInfo::CF_PERCENT},
                   {"Max dur", SectionInfo::CF_DURATION}});
    RETURN_IF_ERROR(EmitPacket(sink, &packet));
  }

  auto it = tp->ExecuteQuery(Sql(scope));
  while (it.Next()) {
    protozero::HeapBuffered<ReportPacket> packet;
    auto* sa = packet->set_slice_aggregate();
    SqlValue name = it.Get(0);
    if (!name.is_null())
      sa->set_name(name.AsString());
    sa->set_count(AsI64(it.Get(1)));
    sa->set_total_dur_ns(AsI64(it.Get(2)));
    sa->set_max_dur_ns(AsI64(it.Get(3)));
    sa->set_pct_of_trace(AsF64(it.Get(4)));
    RETURN_IF_ERROR(EmitPacket(sink, &packet));
  }
  RETURN_IF_ERROR(it.Status());
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
