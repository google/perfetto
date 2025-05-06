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
                                      query.table_name + " AS " + query.sql);
    PERFETTO_CHECK(!it.Next());
    if (!it.Status().ok()) {
      return base::ErrStatus("Error while executing shared query %s: %s",
                             query.id.c_str(), it.Status().c_message());
    }
  }
  protozero::HeapBuffered<protos::pbzero::TraceSummary> summary;
  for (auto m = queries_per_metric.GetIterator(); m; ++m) {
    auto metric_name = m.key().c_str();
    if (m.value().query.empty()) {
      return base::ErrStatus("Metric %s was not found in any summary spec",
                             metric_name);
    }
    auto* metric = summary->add_metric();
    metric->AppendBytes(protos::pbzero::TraceMetricV2::kSpecFieldNumber,
                        m.value().spec.data, m.value().spec.size);

    auto it = processor->ExecuteQuery(m.value().query);
    protos::pbzero::TraceMetricV2Spec::Decoder spec_decoder(
        m.value().spec.data, m.value().spec.size);

    uint32_t col_count = it.ColumnCount();
    base::FlatHashMap<std::string, uint32_t> column_name_to_index;
    for (uint32_t i = 0; i < col_count; ++i) {
      column_name_to_index.Insert(it.GetColumnName(i), i);
    }
    std::string metric_value_column_name = spec_decoder.value().ToStdString();
    auto* metric_value_index =
        column_name_to_index.Find(metric_value_column_name);
    if (!metric_value_index) {
      return base::ErrStatus(
          "Column %s not found in the query result for metric %s",
          metric_value_column_name.c_str(), metric_name);
    }

    std::vector<protos::pbzero::TraceMetricV2Spec::DimensionSpec::Decoder>
        dimension_specs;
    for (auto dim_spec_it = spec_decoder.dimensions_specs(); dim_spec_it;
         ++dim_spec_it) {
      dimension_specs.emplace_back(*dim_spec_it);
    }
    while (it.Next()) {
      PERFETTO_CHECK(col_count > 0);
      const auto& metric_value_column = it.Get(*metric_value_index);
      // Skip null rows.
      if (metric_value_column.is_null()) {
        PERFETTO_DLOG(
            "Skipping row for metric %s because the value column was null",
            metric_name);
        continue;
      }

      auto* row = metric->add_row();
      // Read metric value.
      switch (metric_value_column.type) {
        case SqlValue::kLong:
          row->set_value(static_cast<double>(metric_value_column.AsLong()));
          break;
        case SqlValue::kDouble:
          row->set_value(metric_value_column.AsDouble());
          break;
        case SqlValue::kNull:
          PERFETTO_FATAL("Null value should have been skipped");
        case SqlValue::kString:
          return base::ErrStatus(
              "Received string for value column in metric %s: this is not "
              "supported",
              metric_name);
        case SqlValue::kBytes:
          return base::ErrStatus(
              "Received bytes for metric value in metric %s: this is not "
              "supported",
              metric_name);
      }

      if (dimension_specs.empty()) {
        // Dimensions are defined without an explicit type
        // Infer the type from the sql value.
        for (auto dim_name_it = spec_decoder.dimensions(); dim_name_it;
             ++dim_name_it) {
          protos::pbzero::TraceMetricV2::MetricRow::Dimension* dimension =
              row->add_dimension();
          auto* dim_index =
              column_name_to_index.Find(dim_name_it->as_std_string());
          if (!dim_index) {
            return base::ErrStatus(
                "Column %s not found in the query result for metric %s",
                dim_name_it->as_std_string().c_str(), metric_name);
          }
          const auto& dimension_value = it.Get(*dim_index);
          switch (dimension_value.type) {
            case SqlValue::kNull:
              dimension->set_null_value();
              break;
            case SqlValue::kLong:
              dimension->set_int64_value(dimension_value.AsLong());
              break;
            case SqlValue::kDouble:
              dimension->set_double_value(dimension_value.AsDouble());
              break;
            case SqlValue::kString:
              dimension->set_string_value(dimension_value.AsString());
              break;
            case SqlValue::kBytes:
              return base::ErrStatus(
                  "Received bytes for dimension in metric %s: this is not "
                  "supported",
                  metric_name);
          }
        }
      } else {
        for (uint32_t i = 0; i < dimension_specs.size(); ++i) {
          protos::pbzero::TraceMetricV2::MetricRow::Dimension* dimension =
              row->add_dimension();

          const auto& dim_spec = dimension_specs[i];
          std::string dim_name = dim_spec.name().ToStdString();
          protos::pbzero::TraceMetricV2Spec::DimensionType dimension_type =
              static_cast<protos::pbzero::TraceMetricV2Spec::DimensionType>(
                  dim_spec.type());

          auto* dim_index = column_name_to_index.Find(dim_name);
          if (!dim_index) {
            return base::ErrStatus(
                "Column %s not found in the query result for metric %s",
                dim_name.c_str(), metric_name);
          }
          const auto& dimension_value = it.Get(*dim_index);
          if (dimension_value.is_null()) {
            // Accept null value for all dimension types.
            dimension->set_null_value();
            continue;
          }
          switch (dimension_type) {
            case protos::pbzero::TraceMetricV2Spec::STRING:
              if (dimension_value.type != SqlValue::kString) {
                return base::ErrStatus(
                    "Expected string for dimension %s in metric %s, got %d",
                    dim_name.c_str(), metric_name, dimension_value.type);
              }
              dimension->set_string_value(dimension_value.AsString());
              break;
            case protos::pbzero::TraceMetricV2Spec::INT64:
              if (dimension_value.type != SqlValue::kLong) {
                return base::ErrStatus(
                    "Expected int64 for dimension %s in metric %s, got %d",
                    dim_name.c_str(), metric_name, dimension_value.type);
              }
              dimension->set_int64_value(dimension_value.AsLong());
              break;
            case protos::pbzero::TraceMetricV2Spec::DOUBLE:
              if (dimension_value.type != SqlValue::kDouble) {
                return base::ErrStatus(
                    "Expected double for dimension %s in metric %s, got %d",
                    dim_name.c_str(), metric_name, dimension_value.type);
              }
              dimension->set_double_value(dimension_value.AsDouble());
              break;
            case protos::pbzero::TraceMetricV2Spec::DIMENSION_TYPE_UNSPECIFIED:
              return base::ErrStatus(
                  "Unsupported dimension type %d for dimension %s in metric %s",
                  dimension_type, dim_name.c_str(), metric_name);
          }
        }
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
