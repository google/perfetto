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

#include "src/trace_processor/trace_summary/summary.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/protozero/text_to_proto/text_to_proto.h"
#include "src/trace_processor/perfetto_sql/generator/structured_query_generator.h"
#include "src/trace_processor/trace_summary/trace_summary.descriptor.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/protozero_to_text.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/trace_summary/file.pbzero.h"
#include "protos/perfetto/trace_summary/v2_metric.pbzero.h"

namespace perfetto::trace_processor::summary {

namespace {

struct Metric {
  std::string query;
  protozero::ConstBytes spec;
};

using perfetto_sql::generator::StructuredQueryGenerator;

base::Status WriteMetadata(TraceProcessor* processor,
                           const std::string& metadata_sql,
                           protos::pbzero::TraceSummary* summary) {
  auto it = processor->ExecuteQuery(metadata_sql);
  RETURN_IF_ERROR(it.Status());

  uint32_t col_count = it.ColumnCount();

  // This can happen if there is no metadata. Just early return in that case.
  if (col_count == 0) {
    return base::OkStatus();
  }

  // Otherwise we expect a strict schema of (key, value).
  if (col_count != 2 || it.GetColumnName(0) != "key" ||
      it.GetColumnName(1) != "value") {
    return base::ErrStatus(
        "Metadata query did not match schema of (key, value)");
  }
  while (it.Next()) {
    auto key = it.Get(0);
    if (key.type != SqlValue::kString) {
      return base::ErrStatus(
          "Key column in metadata query was not of type string");
    }
    // Silently ignore any null values.
    auto value = it.Get(1);
    if (value.is_null()) {
      continue;
    }
    if (value.type != SqlValue::kString) {
      return base::ErrStatus(
          "Key column in metadata query was not of type string or null");
    }
    auto* metadata = summary->add_metadata();
    metadata->set_key(key.AsString());
    metadata->set_value(value.AsString());
  }
  return it.Status();
}

base::Status CreateQueriesAndComputeMetrics(
    TraceProcessor* processor,
    const DescriptorPool& pool,
    const std::vector<StructuredQueryGenerator::Query>& queries,
    const base::FlatHashMap<std::string, Metric>& queries_per_metric,
    const std::optional<std::string>& metadata_sql,
    std::vector<uint8_t>* output,
    const TraceSummaryOutputSpec& output_spec) {
  for (const auto& query : queries) {
    auto it = processor->ExecuteQuery("CREATE PERFETTO TABLE " +
                                      query.table_name + " " + query.sql);
    PERFETTO_CHECK(!it.Next());
    if (!it.Status().ok()) {
      return base::ErrStatus("Error while executing shared query %s: %s",
                             query.id.c_str(), it.Status().c_message());
    }
  }
  protozero::HeapBuffered<protos::pbzero::TraceSummary> summary;
  for (auto m = queries_per_metric.GetIterator(); m; ++m) {
    if (m.value().query.empty()) {
      return base::ErrStatus("Metric %s was not found in any summary spec",
                             m.key().c_str());
    }
    auto* metric = summary->add_metric();
    metric->AppendBytes(protos::pbzero::TraceMetricV2::kSpecFieldNumber,
                        m.value().spec.data, m.value().spec.size);

    auto it = processor->ExecuteQuery(m.value().query);
    uint32_t col_count = it.ColumnCount();
    while (it.Next()) {
      PERFETTO_CHECK(col_count > 0);
      const auto& value = it.Get(col_count - 1);

      // Skip null rows.
      if (value.is_null()) {
        continue;
      }

      auto* row = metric->add_row();
      for (uint32_t i = 0; i < col_count - 1; ++i) {
        const auto& dim = it.Get(i);
        switch (dim.type) {
          case SqlValue::kLong:
            row->add_dimension()->set_int64_value(dim.AsLong());
            break;
          case SqlValue::kDouble:
            row->add_dimension()->set_double_value(dim.AsDouble());
            break;
          case SqlValue::kString:
            row->add_dimension()->set_string_value(dim.AsString());
            break;
          case SqlValue::kNull:
            row->add_dimension()->set_null_value();
            break;
          case SqlValue::kBytes:
            return base::ErrStatus(
                "Received bytes for dimension in metric %s: this is not "
                "supported",
                m.key().c_str());
        }
      }

      switch (value.type) {
        case SqlValue::kLong:
          row->set_value(static_cast<double>(value.AsLong()));
          break;
        case SqlValue::kDouble:
          row->set_value(value.AsDouble());
          break;
        case SqlValue::kNull:
          PERFETTO_FATAL("Null value should have been skipped");
        case SqlValue::kString:
          return base::ErrStatus(
              "Received string for metric value in metric %s: this is not "
              "supported",
              m.key().c_str());
        case SqlValue::kBytes:
          return base::ErrStatus(
              "Received bytes for metric value in metric %s: this is not "
              "supported",
              m.key().c_str());
      }
    }
    RETURN_IF_ERROR(it.Status());
  }
  if (metadata_sql) {
    RETURN_IF_ERROR(WriteMetadata(processor, *metadata_sql, summary.get()));
  }
  switch (output_spec.format) {
    case TraceSummaryOutputSpec::Format::kBinaryProto:
      *output = summary.SerializeAsArray();
      break;
    case TraceSummaryOutputSpec::Format::kTextProto:
      std::vector<uint8_t> proto = summary.SerializeAsArray();
      std::string out = protozero_to_text::ProtozeroToText(
          pool, ".perfetto.protos.TraceSummary",
          protozero::ConstBytes{proto.data(), proto.size()});
      *output = std::vector<uint8_t>(out.begin(), out.end());
      break;
  }
  return base::OkStatus();
}

}  // namespace

base::Status Summarize(TraceProcessor* processor,
                       const DescriptorPool& pool,
                       const TraceSummaryComputationSpec& computation,
                       const std::vector<TraceSummarySpecBytes>& specs,
                       std::vector<uint8_t>* output,
                       const TraceSummaryOutputSpec& output_spec) {
  std::vector<protos::pbzero::TraceSummarySpec::Decoder> spec_decoders;
  std::vector<std::vector<uint8_t>> textproto_converted_specs(specs.size());
  for (uint32_t i = 0; i < specs.size(); ++i) {
    switch (specs[i].format) {
      case TraceSummarySpecBytes::Format::kBinaryProto:
        spec_decoders.emplace_back(specs[i].ptr, specs[i].size);
        break;
      case TraceSummarySpecBytes::Format::kTextProto:
        ASSIGN_OR_RETURN(
            textproto_converted_specs[i],
            protozero::TextToProto(
                kTraceSummaryDescriptor.data(), kTraceSummaryDescriptor.size(),
                ".perfetto.protos.TraceSummarySpec", "-",
                std::string_view(reinterpret_cast<const char*>(specs[i].ptr),
                                 specs[i].size)));
        spec_decoders.emplace_back(textproto_converted_specs[i].data(),
                                   textproto_converted_specs[i].size());
        break;
    }
  }

  perfetto_sql::generator::StructuredQueryGenerator generator;
  for (const auto& spec : spec_decoders) {
    for (auto it = spec.query(); it; ++it) {
      RETURN_IF_ERROR(generator.AddQuery(it->data(), it->size()));
    }
  }

  base::FlatHashMap<std::string, Metric> queries_per_metric;
  if (!computation.v2_metric_ids.empty()) {
    for (const auto& id : computation.v2_metric_ids) {
      queries_per_metric.Insert(id, Metric{});
    }
    for (const auto& spec : spec_decoders) {
      for (auto it = spec.metric_spec(); it; ++it) {
        protos::pbzero::TraceMetricV2Spec::Decoder m(*it);
        std::string id = m.id().ToStdString();
        if (id.empty()) {
          return base::ErrStatus(
              "Metric with empty id field: this is not allowed");
        }

        // Only compute metrics which were populated in the map (i.e. the ones
        // which were specified in the `computation.v2_metric_ids` field).
        Metric* metric = queries_per_metric.Find(id);
        if (!metric) {
          continue;
        }
        if (!metric->query.empty()) {
          return base::ErrStatus(
              "Duplicate definitions for metric %s received: this is not "
              "allowed",
              id.c_str());
        }
        base::StatusOr<std::string> query_or =
            generator.Generate(m.query().data, m.query().size);
        if (!query_or.ok()) {
          return base::ErrStatus("Unable to build query for metric %s: %s",
                                 id.c_str(), query_or.status().c_message());
        }
        metric->query = *query_or;
        metric->spec = protozero::ConstBytes{
            m.begin(),
            static_cast<size_t>(m.end() - m.begin()),
        };
      }
    }
  }

  std::optional<std::string> metadata_sql;
  if (computation.metadata_query_id) {
    ASSIGN_OR_RETURN(metadata_sql,
                     generator.GenerateById(*computation.metadata_query_id));
  }

  for (const auto& module : generator.ComputeReferencedModules()) {
    auto it = processor->ExecuteQuery("INCLUDE PERFETTO MODULE " + module);
    PERFETTO_CHECK(!it.Next());
    RETURN_IF_ERROR(it.Status());
  }

  auto queries = generator.referenced_queries();
  base::Status status = CreateQueriesAndComputeMetrics(
      processor, pool, queries, queries_per_metric, metadata_sql, output,
      output_spec);

  // Make sure to cleanup all the queries.
  for (const auto& query : queries) {
    auto it =
        processor->ExecuteQuery("DROP TABLE IF EXISTS " + query.table_name);
    PERFETTO_CHECK(!it.Next());
    PERFETTO_CHECK(it.Status().ok());
  }
  return status;
}

}  // namespace perfetto::trace_processor::summary
