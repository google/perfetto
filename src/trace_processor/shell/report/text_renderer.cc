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

#include "src/trace_processor/shell/report/text_renderer.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace_processor/report.pbzero.h"

namespace perfetto::trace_processor::shell {
namespace {

constexpr size_t kMaxNameWidth = 50;

std::string FormatDuration(int64_t ns) {
  double a = std::abs(static_cast<double>(ns));
  if (a >= 1e9)
    return base::StackString<32>("%.1fs", static_cast<double>(ns) / 1e9)
        .ToStdString();
  if (a >= 1e6)
    return base::StackString<32>("%.0fms", static_cast<double>(ns) / 1e6)
        .ToStdString();
  if (a >= 1e3)
    return base::StackString<32>("%.0fus", static_cast<double>(ns) / 1e3)
        .ToStdString();
  return base::StackString<32>("%" PRId64 "ns", ns).ToStdString();
}

std::string FormatCount(int64_t n) {
  double a = std::abs(static_cast<double>(n));
  if (a >= 1e6)
    return base::StackString<32>("%.1fM", static_cast<double>(n) / 1e6)
        .ToStdString();
  if (a >= 1e3)
    return base::StackString<32>("%.1fk", static_cast<double>(n) / 1e3)
        .ToStdString();
  return base::StackString<32>("%" PRId64, n).ToStdString();
}

std::string FormatPercent(double pct) {
  return base::StackString<32>("%.1f%%", pct).ToStdString();
}

std::string CapName(const std::string& name) {
  if (name.size() <= kMaxNameWidth)
    return name;
  return name.substr(0, kMaxNameWidth - 3) + "...";
}

// Renders a raw cell to a display string per its column's declared format.
std::string FormatCell(const TextSink::Cell& c, int32_t format) {
  using SI = protos::pbzero::SectionInfo;
  switch (format) {
    case SI::CF_INT:
      return base::StackString<32>("%" PRId64, c.i).ToStdString();
    case SI::CF_COUNT:
      return FormatCount(c.i);
    case SI::CF_DURATION:
      return FormatDuration(c.i);
    case SI::CF_PERCENT:
      return FormatPercent(c.d);
    case SI::CF_STRING:
    default:
      return c.str;
  }
}

}  // namespace

TextSink::TextSink(FILE* out, bool overview) : out_(out), overview_(overview) {}

TextSink::~TextSink() = default;

base::Status TextSink::OnPacket(protozero::ConstBytes packet) {
  protos::pbzero::ReportPacket::Decoder p(packet.data, packet.size);
  if (p.has_header()) {
    auto hb = p.header();
    protos::pbzero::ReportHeader::Decoder h(hb.data, hb.size);
    trace_file_ = h.trace_file().ToStdString();
    std::string name = trace_file_.empty() ? "trace" : trace_file_;
    fprintf(out_, "[%s | full trace | %s | %" PRId64 " processes]\n",
            name.c_str(), FormatDuration(h.trace_dur_ns()).c_str(),
            h.process_count());
    return base::OkStatus();
  }
  if (p.has_section_info()) {
    FlushSection();
    auto sb = p.section_info();
    protos::pbzero::SectionInfo::Decoder s(sb.data, sb.size);
    in_section_ = true;
    section_title_ = s.title().ToStdString();
    row_noun_ = s.row_noun().ToStdString();
    total_rows_ = s.total_rows();
    shown_rows_ = s.shown_rows();
    total_items_ = s.total_items();
    // The section declares its own columns; the renderer takes its headers and
    // per-column formatting from the stream rather than from the noun.
    columns_.clear();
    column_formats_.clear();
    for (auto it = s.columns(); it; ++it) {
      protos::pbzero::SectionInfo::Column::Decoder col(*it);
      columns_.push_back(col.name().ToStdString());
      column_formats_.push_back(col.format());
    }
    rows_.clear();
    return base::OkStatus();
  }
  if (p.has_slice_aggregate()) {
    auto sb = p.slice_aggregate();
    protos::pbzero::SliceAggregate::Decoder s(sb.data, sb.size);
    rows_.push_back({Cell::Str(CapName(s.name().ToStdString())),
                     Cell::Int(s.count()), Cell::Int(s.total_dur_ns()),
                     Cell::Dbl(s.pct_of_trace()), Cell::Int(s.max_dur_ns())});
    return base::OkStatus();
  }
  return base::OkStatus();
}

void TextSink::FlushSection() {
  if (!in_section_)
    return;
  in_section_ = false;

  std::string headline = section_title_;
  if (total_items_ > 0)
    headline += " (" + FormatCount(total_items_) + " total)";
  fprintf(out_, "\n%s:\n", headline.c_str());

  // Format the raw cells to strings up front, keyed by each column's format.
  std::vector<std::vector<std::string>> cells;
  cells.reserve(rows_.size());
  for (const auto& row : rows_) {
    std::vector<std::string> out;
    out.reserve(row.size());
    for (size_t c = 0; c < row.size(); ++c) {
      int32_t fmt = c < column_formats_.size() ? column_formats_[c] : 0;
      out.push_back(FormatCell(row[c], fmt));
    }
    cells.push_back(std::move(out));
  }

  std::vector<size_t> widths(columns_.size());
  for (size_t c = 0; c < columns_.size(); ++c)
    widths[c] = columns_[c].size();
  for (const auto& row : cells) {
    for (size_t c = 0; c < row.size(); ++c)
      widths[c] = std::max(widths[c], row[c].size());
  }

  auto print_row = [&](const std::vector<std::string>& row) {
    std::string line = "  ";
    for (size_t c = 0; c < row.size(); ++c) {
      line += row[c];
      if (c + 1 < row.size())
        line.append(widths[c] - row[c].size() + 3, ' ');
    }
    fprintf(out_, "%s\n", line.c_str());
  };

  print_row(columns_);
  for (const auto& row : cells)
    print_row(row);

  if (total_rows_ > shown_rows_) {
    int64_t hidden = total_rows_ - shown_rows_;
    int64_t suggested = std::min<int64_t>(
        total_rows_, shown_rows_ < 50 ? 50 : shown_rows_ * 5);
    std::string noun = row_noun_.empty() ? "rows" : row_noun_;
    fprintf(out_,
            "  * %" PRId64 " more %s below --top %" PRId64
            "; rerun with --top %" PRId64 "\n",
            hidden, noun.c_str(), shown_rows_, suggested);
  }
}

base::Status TextSink::Finalize() {
  FlushSection();
  if (overview_) {
    std::string trace = trace_file_.empty() ? "<trace>" : trace_file_;
    fprintf(out_, "\nNext: report slices %s --top 50\n", trace.c_str());
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
