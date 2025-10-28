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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/fnv_hash.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/protozero/text_to_proto/text_to_proto.h"
#include "src/trace_processor/perfetto_sql/generator/structured_query_generator.h"
#include "src/trace_processor/trace_summary/trace_summary.descriptor.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/protozero_to_text.h"

#include "protos/perfetto/trace_summary/file.pbzero.h"
#include "protos/perfetto/trace_summary/v2_metric.pbzero.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
#include <zlib.h>
#endif

namespace perfetto::trace_processor::summary {

namespace {

using perfetto_sql::generator::StructuredQueryGenerator;
using TraceMetricV2Spec = protos::pbzero::TraceMetricV2Spec;
using TraceMetricV2TemplateSpec = protos::pbzero::TraceMetricV2TemplateSpec;
using TraceMetricV2Bundle = protos::pbzero::TraceMetricV2Bundle;
using TraceSummarySpec = protos::pbzero::TraceSummarySpec;
using TraceSummary = protos::pbzero::TraceSummary;
using PerfettoSqlStructuredQuery = protos::pbzero::PerfettoSqlStructuredQuery;
using InternedDimensionSpec = TraceMetricV2Spec::InternedDimensionSpec;

base::StatusOr<uint64_t> HashOf(const SqlValue& val) {
  base::FnvHasher hasher;
  hasher.Update(val.type);
  if (val.is_null()) {
  } else if (val.type == SqlValue::kLong) {
    hasher.Update(val.long_value);
  } else if (val.type == SqlValue::kDouble) {
    hasher.Update(val.double_value);
  } else if (val.type == SqlValue::kString) {
    hasher.Update(val.string_value);
  } else {
    return base::ErrStatus("Unsupported SqlValue type %d for hashing",
                           static_cast<int>(val.type));
  }
  return hasher.digest();
}

struct Metric {
  std::string id;
  std::string query;
  protozero::ConstBytes spec;
  std::vector<std::string> interned_dimension_queries;
};

base::Status ExpandMetricTemplates(
    const std::vector<TraceSummarySpec::Decoder>& spec_decoders,
    std::vector<std::vector<uint8_t>>& synthetic_protos) {
  protozero::HeapBuffered<TraceMetricV2Spec> expanded;
  for (const auto& spec : spec_decoders) {
    for (auto it = spec.metric_template_spec(); it; ++it) {
      TraceMetricV2TemplateSpec::Decoder tmpl(*it);
      std::string id_prefix = tmpl.id_prefix().ToStdString();
      if (id_prefix.empty()) {
        return base::ErrStatus(
            "Metric template with empty id_prefix field: this is not allowed");
      }
      if (tmpl.has_value_columns() && tmpl.has_value_column_specs()) {
        return base::ErrStatus(
            "Metric template has both value_columns and value_column_specs "
            "defined: this is not allowed");
      }

      struct ValueColumnInfo {
        std::string name;
        std::optional<TraceMetricV2Spec::MetricUnit> unit;
        std::string custom_unit;
        std::optional<TraceMetricV2Spec::MetricPolarity> polarity;
      };
      std::vector<ValueColumnInfo> value_column_infos;
      if (tmpl.has_value_columns()) {
        for (auto vc_it = tmpl.value_columns(); vc_it; ++vc_it) {
          value_column_infos.push_back(
              {vc_it->as_std_string(), std::nullopt, "", std::nullopt});
        }
      } else {
        for (auto vcs_it = tmpl.value_column_specs(); vcs_it; ++vcs_it) {
          TraceMetricV2TemplateSpec::ValueColumnSpec::Decoder value_spec(
              *vcs_it);
          std::optional<TraceMetricV2Spec::MetricUnit> unit;
          if (value_spec.has_unit()) {
            unit =
                static_cast<TraceMetricV2Spec::MetricUnit>(value_spec.unit());
          }
          std::optional<TraceMetricV2Spec::MetricPolarity> polarity;
          if (value_spec.has_polarity()) {
            polarity = static_cast<TraceMetricV2Spec::MetricPolarity>(
                value_spec.polarity());
          }
          value_column_infos.push_back({value_spec.name().ToStdString(), unit,
                                        value_spec.custom_unit().ToStdString(),
                                        polarity});
        }
      }

      for (const auto& info : value_column_infos) {
        expanded.Reset();
        expanded->set_id(id_prefix + "_" + info.name);
        expanded->set_value(info.name);
        if (info.unit) {
          expanded->set_unit(*info.unit);
        }
        if (!info.custom_unit.empty()) {
          expanded->set_custom_unit(info.custom_unit);
        }
        if (info.polarity) {
          expanded->set_polarity(*info.polarity);
        }
        for (auto dim = tmpl.dimensions(); dim; ++dim) {
          protozero::ConstChars dim_str = *dim;
          expanded->add_dimensions(dim_str.data, dim_str.size);
        }
        for (auto dim = tmpl.dimensions_specs(); dim; ++dim) {
          protozero::ConstBytes dim_spec = *dim;
          expanded->add_dimensions_specs()->AppendRawProtoBytes(dim_spec.data,
                                                                dim_spec.size);
        }
        if (tmpl.has_query()) {
          expanded->set_query()->AppendRawProtoBytes(tmpl.query().data,
                                                     tmpl.query().size);
        }
        if (!tmpl.disable_auto_bundling()) {
          expanded->set_bundle_id(id_prefix);
        }
        if (tmpl.has_dimension_uniqueness()) {
          expanded->set_dimension_uniqueness(
              static_cast<TraceMetricV2Spec::DimensionUniqueness>(
                  tmpl.dimension_uniqueness()));
        }

        for (auto ms_it = tmpl.interned_dimension_specs(); ms_it; ++ms_it) {
          expanded->add_interned_dimension_specs()->AppendRawProtoBytes(
              ms_it->data(), ms_it->size());
        }

        synthetic_protos.push_back(expanded.SerializeAsArray());
      }
    }
  }
  return base::OkStatus();
}

base::Status WriteMetadata(TraceProcessor* processor,
                           const std::string& metadata_sql,
                           TraceSummary* summary) {
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

struct Dimension {
  std::string name;
  TraceMetricV2Spec::DimensionType type;

  [[maybe_unused]] bool operator==(const Dimension& other) const {
    return std::tie(name, type) == std::tie(other.name, other.type);
  }
  [[maybe_unused]] bool operator!=(const Dimension& other) const {
    return !(*this == other);
  }
};

base::StatusOr<std::vector<Dimension>> GetDimensions(
    const TraceMetricV2Spec::Decoder& spec_decoder) {
  if (spec_decoder.has_dimensions_specs() && spec_decoder.has_dimensions()) {
    return base::ErrStatus(
        "Both dimensions and dimension_specs defined for metric '%s'. Only "
        "one is allowed",
        spec_decoder.id().ToStdString().c_str());
  }
  std::vector<Dimension> dimensions;
  if (spec_decoder.dimensions_specs()) {
    for (auto dim = spec_decoder.dimensions_specs(); dim; ++dim) {
      TraceMetricV2Spec::DimensionSpec::Decoder dim_spec(*dim);
      std::string_view dim_name = dim_spec.name().ToStdStringView();
      auto dimension_type =
          static_cast<TraceMetricV2Spec::DimensionType>(dim_spec.type());
      if (dimension_type == TraceMetricV2Spec::DIMENSION_TYPE_UNSPECIFIED) {
        return base::ErrStatus(
            "Dimension '%.*s' in metric '%s' has unspecified type",
            int(dim_name.size()), dim_name.data(),
            spec_decoder.id().ToStdString().c_str());
      }
      dimensions.push_back({dim_spec.name().ToStdString(), dimension_type});
    }
  } else {
    for (auto dim = spec_decoder.dimensions(); dim; ++dim) {
      dimensions.push_back({dim->as_std_string(),
                            TraceMetricV2Spec::DIMENSION_TYPE_UNSPECIFIED});
    }
  }
  return dimensions;
}

struct DimensionWithIndex : Dimension {
  uint32_t index;
};

base::StatusOr<std::vector<DimensionWithIndex>> GetDimensionsWithIndex(
    const TraceMetricV2Spec::Decoder& spec_decoder,
    perfetto::trace_processor::Iterator& it) {
  ASSIGN_OR_RETURN(auto dimensions, GetDimensions(spec_decoder));
  std::vector<DimensionWithIndex> output;
  uint32_t col_count = it.ColumnCount();
  for (const auto& dim : dimensions) {
    std::optional<uint32_t> dim_index;
    for (uint32_t i = 0; i < col_count; ++i) {
      if (it.GetColumnName(i) == dim.name) {
        dim_index = i;
        break;
      }
    }
    if (!dim_index) {
      return base::ErrStatus(
          "Dimensions column '%s' not found in the query result for metric "
          "'%s'",
          dim.name.c_str(), spec_decoder.id().ToStdString().c_str());
    }
    output.push_back({dim, *dim_index});
  }
  return output;
}

base::Status WriteDimension(
    const DimensionWithIndex& dim_with_index,
    const std::string& metric_or_bundle_name,
    Iterator& query_it,
    protos::pbzero::TraceMetricV2Bundle::Row::Dimension* dimension,
    base::FnvHasher* hasher) {
  const auto& dimension_value = query_it.Get(dim_with_index.index);
  hasher->Update(dimension_value.type);
  if (dimension_value.is_null()) {
    // Accept null value for all dimension types.
    dimension->set_null_value();
    return base::OkStatus();
  }
  switch (dim_with_index.type) {
    case TraceMetricV2Spec::STRING: {
      if (dimension_value.type != SqlValue::kString) {
        return base::ErrStatus(
            "Expected string for dimension '%s' in metric or bundle '%s', got "
            "%d",
            dim_with_index.name.c_str(), metric_or_bundle_name.c_str(),
            dimension_value.type);
      }
      const char* dimension_str = dimension_value.string_value;
      hasher->Update(dimension_str);
      dimension->set_string_value(dimension_str);
      break;
    }
    case TraceMetricV2Spec::INT64: {
      if (dimension_value.type != SqlValue::kLong) {
        return base::ErrStatus(
            "Expected int64 for dimension '%s' in metric or bundle '%s', got "
            "%d",
            dim_with_index.name.c_str(), metric_or_bundle_name.c_str(),
            dimension_value.type);
      }
      int64_t dim_value = dimension_value.long_value;
      hasher->Update(dim_value);
      dimension->set_int64_value(dim_value);
      break;
    }
    case TraceMetricV2Spec::DOUBLE: {
      if (dimension_value.type != SqlValue::kDouble) {
        return base::ErrStatus(
            "Expected double for dimension '%s' in metric or bundle '%s', got "
            "%d",
            dim_with_index.name.c_str(), metric_or_bundle_name.c_str(),
            dimension_value.type);
      }
      double dim_value = dimension_value.AsDouble();
      hasher->Update(dim_value);
      dimension->set_double_value(dim_value);
      break;
    }
    case TraceMetricV2Spec::BOOLEAN: {
      if (dimension_value.type != SqlValue::kLong) {
        return base::ErrStatus(
            "Expected bool for dimension '%s' in metric or bundle '%s', got "
            "%d",
            dim_with_index.name.c_str(), metric_or_bundle_name.c_str(),
            dimension_value.type);
      }
      if (dimension_value.long_value != 0 && dimension_value.long_value != 1) {
        return base::ErrStatus(
            "Expected bool 0 or 1 for dimension '%s' in metric or bundle '%s', "
            "got %" PRId64,
            dim_with_index.name.c_str(), metric_or_bundle_name.c_str(),
            dimension_value.long_value);
      }
      bool dim_value = dimension_value.long_value != 0;
      hasher->Update(dim_value);
      dimension->set_bool_value(dim_value);
      break;
    }
    case TraceMetricV2Spec::DIMENSION_TYPE_UNSPECIFIED:
      if (dimension_value.type == SqlValue::kLong) {
        int64_t dim_value = dimension_value.long_value;
        hasher->Update(dim_value);
        dimension->set_int64_value(dim_value);
      } else if (dimension_value.type == SqlValue::kDouble) {
        double dim_value = dimension_value.AsDouble();
        hasher->Update(dim_value);
        dimension->set_double_value(dim_value);
      } else if (dimension_value.type == SqlValue::kString) {
        const char* dimension_str = dimension_value.string_value;
        hasher->Update(dimension_str);
        dimension->set_string_value(dimension_str);
      } else if (dimension_value.type == SqlValue::kBytes) {
        return base::ErrStatus(
            "Received bytes for dimension '%s' in metric or bundle '%s': this "
            "is not supported",
            dim_with_index.name.c_str(), metric_or_bundle_name.c_str());
      } else {
        PERFETTO_FATAL("Null dimension should have been handled above");
      }
      break;
  }
  return base::OkStatus();
}

struct InternedDimensionKey {
  std::string key_column_name;
  uint64_t key_hash;
  bool operator==(const InternedDimensionKey& o) const {
    return key_column_name == o.key_column_name && key_hash == o.key_hash;
  }
  struct Hasher {
    size_t operator()(const InternedDimensionKey& k) const {
      base::FnvHasher h;
      h.Update(k.key_column_name.data(), k.key_column_name.size());
      h.Update(k.key_hash);
      return static_cast<size_t>(h.digest());
    }
  };
};

base::Status WriteInternedDimensionValue(
    const SqlValue& col_value,
    TraceMetricV2Spec::DimensionType type,
    TraceMetricV2Bundle::Row::Dimension* value) {
  if (col_value.is_null()) {
    value->set_null_value();
    return base::OkStatus();
  }
  switch (type) {
    case TraceMetricV2Spec::DimensionType::STRING: {
      if (col_value.type != SqlValue::kString) {
        return base::ErrStatus("Expected string for interned dimension column");
      }
      value->set_string_value(col_value.string_value);
      break;
    }
    case TraceMetricV2Spec::DimensionType::INT64: {
      if (col_value.type != SqlValue::kLong) {
        return base::ErrStatus("Expected int64 for interned dimension column");
      }
      value->set_int64_value(col_value.long_value);
      break;
    }
    case TraceMetricV2Spec::DimensionType::DOUBLE: {
      if (col_value.type != SqlValue::kDouble) {
        return base::ErrStatus("Expected double for interned dimension column");
      }
      value->set_double_value(col_value.double_value);
      break;
    }
    case TraceMetricV2Spec::DimensionType::BOOLEAN: {
      if (col_value.type != SqlValue::kLong) {
        return base::ErrStatus("Expected bool for interned dimension column");
      }
      if (col_value.long_value != 0 && col_value.long_value != 1) {
        return base::ErrStatus(
            "Expected bool 0 or 1 for interned dimension column, got %" PRId64,
            col_value.long_value);
      }
      value->set_bool_value(col_value.long_value != 0);
      break;
    }
    case TraceMetricV2Spec::DimensionType::DIMENSION_TYPE_UNSPECIFIED:
      PERFETTO_FATAL("Unspecified type for interned dimension column");
      break;
  }
  return base::OkStatus();
}

base::Status VerifyBundleHasConsistentSpecs(
    const std::string& bundle_id,
    const std::vector<const Metric*>& metrics) {
  if (metrics.empty()) {
    return base::ErrStatus("Empty metric bundle %s: this is not allowed",
                           bundle_id.c_str());
  }
  if (metrics.size() == 1) {
    return base::OkStatus();
  }
  const Metric* first = metrics.front();
  TraceMetricV2Spec::Decoder first_spec(first->spec);
  ASSIGN_OR_RETURN(auto first_dims, GetDimensions(first_spec));
  for (const Metric* metric : metrics) {
    TraceMetricV2Spec::Decoder spec(metric->spec);
    if (spec.bundle_id().ToStdStringView() !=
        first_spec.bundle_id().ToStdStringView()) {
      return base::ErrStatus(
          "Metric '%s' in bundle '%s' has different bundle_id than the "
          "first metric '%s': this is not allowed",
          metric->id.c_str(), bundle_id.c_str(), first->id.c_str());
    }
    if (spec.dimension_uniqueness() != first_spec.dimension_uniqueness()) {
      return base::ErrStatus(
          "Metric '%s' in bundle '%s' has different dimension_uniqueness than "
          "the first metric '%s': this is not allowed",
          metric->id.c_str(), bundle_id.c_str(), first->id.c_str());
    }
    ASSIGN_OR_RETURN(auto dims, GetDimensions(spec));
    if (dims != first_dims) {
      return base::ErrStatus(
          "Metric '%s' in bundle '%s' has different dimensions than the first "
          "metric '%s': this is not allowed",
          metric->id.c_str(), bundle_id.c_str(), first->id.c_str());
    }
    if (first->query != metric->query) {
      return base::ErrStatus(
          "Metric '%s' in bundle '%s' has different query than the first "
          "metric '%s': this is not allowed",
          metric->id.c_str(), bundle_id.c_str(), first->id.c_str());
    }
  }
  return base::OkStatus();
}

base::Status ValidateInternedDimensionSpecs(
    const TraceMetricV2Spec::Decoder& spec) {
  ASSIGN_OR_RETURN(auto dimensions, GetDimensions(spec));
  std::set<std::string> interned_dimension_keys;
  for (auto it = spec.interned_dimension_specs(); it; ++it) {
    InternedDimensionSpec::Decoder ms(*it);
    if (!ms.has_key_column_spec()) {
      return base::ErrStatus(
          "key_column_spec must be specified in interned_dimension_specs");
    }
    InternedDimensionSpec::ColumnSpec::Decoder key_col_spec(
        ms.key_column_spec());
    base::StringView key_column_name = key_col_spec.name();
    if (!interned_dimension_keys.insert(key_column_name.ToStdString()).second) {
      return base::ErrStatus(
          "Duplicate key column '%.*s' found in interned_dimension_specs",
          static_cast<int>(key_column_name.size()), key_column_name.data());
    }

    bool key_in_dimensions = false;
    for (const auto& dim : dimensions) {
      if (base::StringView(dim.name) == key_column_name) {
        key_in_dimensions = true;
        break;
      }
    }
    if (!key_in_dimensions) {
      return base::ErrStatus(
          "Key column '%.*s' in interned dimension bundle not found in metric "
          "dimensions",
          static_cast<int>(key_column_name.size()), key_column_name.data());
    }

    std::set<std::string> all_column_names;
    all_column_names.insert(key_column_name.ToStdString());
    for (auto dcs_it = ms.data_column_specs(); dcs_it; ++dcs_it) {
      InternedDimensionSpec::ColumnSpec::Decoder data_col_spec(*dcs_it);
      base::StringView data_col_name = data_col_spec.name();
      if (!all_column_names.insert(data_col_name.ToStdString()).second) {
        return base::ErrStatus(
            "Duplicate column name '%.*s' found in interned dimension bundle",
            static_cast<int>(data_col_name.size()), data_col_name.data());
      }
    }
  }
  return base::OkStatus();
}

base::Status CheckForDuplicateAndInsert(
    uint64_t hash,
    base::StringView key_column_name,
    std::unordered_set<uint64_t>& seen_keys) {
  if (!seen_keys.insert(hash).second) {
    return base::ErrStatus(
        "Duplicate key found in interned dimension bundle with key column "
        "'%.*s'",
        static_cast<int>(key_column_name.size()), key_column_name.data());
  }
  return base::OkStatus();
}

base::Status WriteInternedDimensionBundles(
    TraceProcessor* processor,
    const TraceMetricV2Spec::Decoder& spec,
    const std::vector<std::string>& interned_dimension_queries,
    const std::unordered_set<InternedDimensionKey,
                             InternedDimensionKey::Hasher>&
        interned_dim_keys_in_metric_bundle,
    TraceMetricV2Bundle* bundle) {
  RETURN_IF_ERROR(ValidateInternedDimensionSpecs(spec));
  size_t interned_dimension_idx = 0;
  for (auto it = spec.interned_dimension_specs(); it; ++it) {
    InternedDimensionSpec::Decoder ms(*it);
    InternedDimensionSpec::ColumnSpec::Decoder key_col_spec(
        ms.key_column_spec());
    base::StringView key_column_name = key_col_spec.name();
    auto* interned_dimension_bundle = bundle->add_interned_dimension_bundles();

    const std::string& sql =
        interned_dimension_queries[interned_dimension_idx++];
    auto query_it = processor->ExecuteQuery(sql);
    if (!query_it.Status().ok()) {
      return base::ErrStatus(
          "Error while executing query for interned dimension bundle with key "
          "'%.*s': %s",
          static_cast<int>(key_column_name.size()), key_column_name.data(),
          query_it.Status().c_message());
    }

    std::vector<protozero::ConstBytes> col_specs_bytes;
    col_specs_bytes.push_back(ms.key_column_spec());
    for (auto dcs_it = ms.data_column_specs(); dcs_it; ++dcs_it) {
      col_specs_bytes.push_back(*dcs_it);
    }

    std::vector<std::pair<uint32_t, TraceMetricV2Spec::DimensionType>>
        column_infos;
    for (const auto& col_spec_bytes : col_specs_bytes) {
      InternedDimensionSpec::ColumnSpec::Decoder col_spec(col_spec_bytes);
      std::optional<uint32_t> column_index;
      for (uint32_t i = 0; i < query_it.ColumnCount(); ++i) {
        if (base::StringView(query_it.GetColumnName(i)) == col_spec.name()) {
          column_index = i;
          break;
        }
      }
      if (!column_index) {
        return base::ErrStatus(
            "Column '%.*s' not found in the query result for interned "
            "dimension "
            "bundle "
            "with key '%.*s'",
            static_cast<int>(col_spec.name().size), col_spec.name().data,
            static_cast<int>(key_column_name.size()), key_column_name.data());
      }
      column_infos.emplace_back(
          *column_index,
          static_cast<TraceMetricV2Spec::DimensionType>(col_spec.type()));
    }

    std::unordered_set<uint64_t> seen_keys;
    while (query_it.Next()) {
      const auto& key_val = query_it.Get(column_infos[0].first);
      ASSIGN_OR_RETURN(uint64_t key_hash, HashOf(key_val));
      // If the key was not in the metric bundle, we don't need to output
      // its interned data.
      if (!interned_dim_keys_in_metric_bundle.count(
              {key_column_name.ToStdString(), key_hash})) {
        continue;
      }
      RETURN_IF_ERROR(
          CheckForDuplicateAndInsert(key_hash, key_column_name, seen_keys));
      auto* row = interned_dimension_bundle->add_interned_dimension_rows();
      RETURN_IF_ERROR(WriteInternedDimensionValue(
          query_it.Get(column_infos[0].first), column_infos[0].second,
          row->set_key_dimension_value()));
      for (size_t i = 1; i < column_infos.size(); ++i) {
        const auto& info = column_infos[i];
        const auto& col_value = query_it.Get(info.first);
        RETURN_IF_ERROR(WriteInternedDimensionValue(
            col_value, info.second, row->add_interned_dimension_values()));
      }
    }
    RETURN_IF_ERROR(query_it.Status());
  }

  return base::OkStatus();
}

base::Status CreateQueriesAndComputeMetrics(TraceProcessor* processor,
                                            const std::vector<Metric>& metrics,
                                            TraceSummary* summary) {
  base::FlatHashMap<std::string, std::vector<const Metric*>> metrics_by_bundle;
  for (const Metric& m : metrics) {
    TraceMetricV2Spec::Decoder spec_decoder(m.spec);
    std::string bundle_id = spec_decoder.bundle_id().ToStdString();
    if (bundle_id.empty()) {
      bundle_id = m.id;
    }
    metrics_by_bundle[bundle_id].push_back(&m);
  }
  for (auto it = metrics_by_bundle.GetIterator(); it; ++it) {
    const auto& value = it.value();
    RETURN_IF_ERROR(VerifyBundleHasConsistentSpecs(it.key(), value));

    const std::string& bundle_id = it.key();
    auto* bundle = summary->add_metric_bundles();
    bundle->set_bundle_id(bundle_id);
    for (const Metric* metric : value) {
      bundle->add_specs()->AppendRawProtoBytes(metric->spec.data,
                                               metric->spec.size);
    }

    const Metric* first = value.front();
    TraceMetricV2Spec::Decoder first_spec(first->spec);

    std::unordered_set<std::string> interned_dim_key_cols;
    for (auto ms_it = first_spec.interned_dimension_specs(); ms_it; ++ms_it) {
      InternedDimensionSpec::Decoder ms_decoder(*ms_it);
      interned_dim_key_cols.insert(InternedDimensionSpec::ColumnSpec::Decoder(
                                       ms_decoder.key_column_spec())
                                       .name()
                                       .ToStdString());
    }
    std::unordered_set<InternedDimensionKey, InternedDimensionKey::Hasher>
        interned_dim_keys_in_metric_bundle;

    auto query_it = processor->ExecuteQuery(first->query);
    if (!query_it.Status().ok()) {
      return base::ErrStatus(
          "Error while executing query for metric bundle '%s': %s",
          bundle_id.c_str(), query_it.Status().c_message());
    }

    PerfettoSqlStructuredQuery::Decoder query(first_spec.query());
    if (query.has_sql()) {
      PerfettoSqlStructuredQuery::Sql::Decoder sql_query(query.sql());
      if (sql_query.has_column_names()) {
        std::set<std::string> actual_column_names;
        for (uint32_t i = 0; i < query_it.ColumnCount(); ++i) {
          actual_column_names.insert(query_it.GetColumnName(i));
        }
        std::set<std::string> expected_column_names;
        for (auto col_it = sql_query.column_names(); col_it; ++col_it) {
          expected_column_names.insert(col_it->as_std_string());
        }
        if (!std::includes(
                actual_column_names.begin(), actual_column_names.end(),
                expected_column_names.begin(), expected_column_names.end())) {
          std::vector<std::string> expected_vec(expected_column_names.begin(),
                                                expected_column_names.end());
          std::vector<std::string> actual_vec(actual_column_names.begin(),
                                              actual_column_names.end());
          return base::ErrStatus(
              "Not all columns expected in metrics bundle '%s' were found. "
              "Expected: [%s], Actual: [%s]",
              bundle_id.c_str(), base::Join(expected_vec, ", ").c_str(),
              base::Join(actual_vec, ", ").c_str());
        }
      }
    }
    ASSIGN_OR_RETURN(std::vector<DimensionWithIndex> dimensions_with_index,
                     GetDimensionsWithIndex(first_spec, query_it));

    std::vector<uint32_t> value_indices;
    for (const auto* metric : value) {
      TraceMetricV2Spec::Decoder spec(metric->spec);
      std::string value_column_name = spec.value().ToStdString();
      std::optional<uint32_t> value_index;
      for (uint32_t i = 0; i < query_it.ColumnCount(); ++i) {
        if (query_it.GetColumnName(i) == value_column_name) {
          value_index = i;
          break;
        }
      }
      if (!value_index) {
        return base::ErrStatus(
            "Column '%s' not found in the query result for metric '%s'",
            value_column_name.c_str(), spec.id().ToStdString().c_str());
      }
      value_indices.push_back(*value_index);
    }
    bool is_unique_dimensions =
        first_spec.dimension_uniqueness() == TraceMetricV2Spec::UNIQUE;
    std::unordered_set<uint64_t> seen_dimensions;
    while (query_it.Next()) {
      bool all_null = true;
      for (size_t i = 0; i < value.size(); ++i) {
        const auto& metric_value_column = query_it.Get(value_indices[i]);
        if (!metric_value_column.is_null()) {
          all_null = false;
          break;
        }
      }
      // If all values are null, we skip writing the row.
      if (all_null) {
        continue;
      }
      auto* row = bundle->add_row();
      base::FnvHasher hasher;
      for (const auto& dim : dimensions_with_index) {
        RETURN_IF_ERROR(WriteDimension(dim, bundle_id, query_it,
                                       row->add_dimension(), &hasher));
        if (interned_dim_key_cols.count(dim.name)) {
          const auto& key_val = query_it.Get(dim.index);
          ASSIGN_OR_RETURN(uint64_t value_hash, HashOf(key_val));
          interned_dim_keys_in_metric_bundle.insert({dim.name, value_hash});
        }
      }
      uint64_t hash = hasher.digest();
      if (is_unique_dimensions && !seen_dimensions.insert(hash).second) {
        return base::ErrStatus(
            "Duplicate dimensions found for metric bundle '%s': this is not "
            "allowed",
            bundle_id.c_str());
      }

      for (size_t i = 0; i < value.size(); ++i) {
        const auto& metric_value_column = query_it.Get(value_indices[i]);
        auto* row_value = row->add_values();
        if (metric_value_column.is_null()) {
          row_value->set_null_value();
          continue;
        }
        switch (metric_value_column.type) {
          case SqlValue::kLong:
            row_value->set_double_value(
                static_cast<double>(metric_value_column.long_value));
            break;
          case SqlValue::kDouble:
            row_value->set_double_value(metric_value_column.double_value);
            break;
          case SqlValue::kNull:
            PERFETTO_FATAL("Null value should have been skipped");
          case SqlValue::kString:
          case SqlValue::kBytes:
            return base::ErrStatus(
                "Received string/bytes for value column in metric '%s': this "
                "is not supported",
                it.value()[i]->id.c_str());
        }
      }
    }
    RETURN_IF_ERROR(query_it.Status());
    if (!first->interned_dimension_queries.empty()) {
      RETURN_IF_ERROR(WriteInternedDimensionBundles(
          processor, first_spec, first->interned_dimension_queries,
          interned_dim_keys_in_metric_bundle, bundle));
    }
  }
  return base::OkStatus();
}

base::Status CreateQueriesAndComputeMetrics(
    TraceProcessor* processor,
    const DescriptorPool& pool,
    const std::vector<StructuredQueryGenerator::Query>& queries,
    const std::vector<Metric>& metrics,
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
  protozero::HeapBuffered<TraceSummary> summary;
  RETURN_IF_ERROR(
      CreateQueriesAndComputeMetrics(processor, metrics, summary.get()));
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

  // Compress the output if requested. If Zlib compression is requested but
  // not supported, return an error.
  if (output_spec.compression == TraceSummaryOutputSpec::Compression::kZlib &&
      !output->empty()) {
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
    auto compressed_size = compressBound(static_cast<uint32_t>(output->size()));
    auto compressed_buffer = std::make_unique<uint8_t[]>(compressed_size);
    int res = compress(compressed_buffer.get(), &compressed_size,
                       output->data(), static_cast<uint32_t>(output->size()));
    if (res != Z_OK) {
      return base::ErrStatus("Failed to compress trace summary output");
    }
    output->assign(
        compressed_buffer.get(),
        compressed_buffer.get() + static_cast<uint32_t>(compressed_size));
#else
    return base::ErrStatus(
        "Zlib compression requested but is not supported on this platform.");
#endif
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
  std::vector<std::vector<uint8_t>> synthetic_protos;
  std::vector<TraceSummarySpec::Decoder> spec_decoders;
  for (uint32_t i = 0; i < specs.size(); ++i) {
    switch (specs[i].format) {
      case TraceSummarySpecBytes::Format::kBinaryProto:
        spec_decoders.emplace_back(specs[i].ptr, specs[i].size);
        break;
      case TraceSummarySpecBytes::Format::kTextProto:
        synthetic_protos.emplace_back();
        ASSIGN_OR_RETURN(
            synthetic_protos.back(),
            protozero::TextToProto(
                kTraceSummaryDescriptor.data(), kTraceSummaryDescriptor.size(),
                ".perfetto.protos.TraceSummarySpec", "-",
                std::string_view(reinterpret_cast<const char*>(specs[i].ptr),
                                 specs[i].size)));
        spec_decoders.emplace_back(synthetic_protos.back().data(),
                                   synthetic_protos.back().size());
        break;
    }
  }

  perfetto_sql::generator::StructuredQueryGenerator generator;
  for (const auto& spec : spec_decoders) {
    for (auto it = spec.query(); it; ++it) {
      RETURN_IF_ERROR(generator.AddQuery(it->data(), it->size()));
    }
  }

  std::vector<TraceMetricV2Spec::Decoder> metric_decoders;
  for (const auto& spec : spec_decoders) {
    for (auto it = spec.metric_spec(); it; ++it) {
      metric_decoders.emplace_back(*it);
    }
  }

  std::vector<std::vector<uint8_t>> expanded_metrics;
  RETURN_IF_ERROR(ExpandMetricTemplates(spec_decoders, expanded_metrics));
  for (const auto& expanded : expanded_metrics) {
    metric_decoders.emplace_back(expanded.data(), expanded.size());
  }

  // If `v2_metric_ids` is an empty vector, we will not compute any metrics.
  std::vector<std::string> metric_ids;
  if (computation.v2_metric_ids) {
    metric_ids = std::move(*computation.v2_metric_ids);
  } else {
    // If `v2_metric_ids` is not specified, we will compute all metrics
    // specified in the summary specs.
    for (const auto& spec : metric_decoders) {
      metric_ids.push_back(spec.id().ToStdString());
    }
  }

  base::FlatHashMap<std::string, size_t> queries_per_metric;
  std::vector<Metric> metrics;
  for (const auto& id : metric_ids) {
    if (base::CaseInsensitiveEqual(id, "all")) {
      return base::ErrStatus(
          "Metric has id 'all' which is not allowed as this is a reserved "
          "name. Please use a different id for your metric");
    }
    queries_per_metric.Insert(id, metrics.size());
    metrics.emplace_back(Metric{id, {}, {}, {}});
  }
  for (const auto& m : metric_decoders) {
    std::string id = m.id().ToStdString();
    if (id.empty()) {
      return base::ErrStatus("Metric with empty id field: this is not allowed");
    }
    // Only compute metrics which were populated in the map (i.e. the ones
    // which were specified in the `computation.v2_metric_ids` field).
    size_t* idx = queries_per_metric.Find(id);
    if (!idx) {
      continue;
    }
    Metric* metric = &metrics[*idx];
    if (!metric->query.empty()) {
      return base::ErrStatus(
          "Duplicate definitions for metric '%s' received: this is not "
          "allowed",
          id.c_str());
    }
    base::StatusOr<std::string> query_or =
        generator.Generate(m.query().data, m.query().size);
    if (!query_or.ok()) {
      return base::ErrStatus("Unable to build query for metric '%s': %s",
                             id.c_str(), query_or.status().c_message());
    }
    metric->query = *query_or;
    metric->spec = protozero::ConstBytes{
        m.begin(),
        static_cast<size_t>(m.end() - m.begin()),
    };
    for (auto it = m.interned_dimension_specs(); it; ++it) {
      InternedDimensionSpec::Decoder ms(*it);
      base::StatusOr<std::string> interned_dimension_query_or =
          generator.Generate(ms.query().data, ms.query().size);
      if (!interned_dimension_query_or.ok()) {
        return base::ErrStatus(
            "Unable to build interned dimension query for metric '%s': %s",
            id.c_str(), interned_dimension_query_or.status().c_message());
      }
      metric->interned_dimension_queries.push_back(
          *interned_dimension_query_or);
    }
  }

  std::optional<std::string> metadata_sql;
  if (computation.metadata_query_id) {
    ASSIGN_OR_RETURN(metadata_sql,
                     generator.GenerateById(*computation.metadata_query_id));
  }

  for (const auto& preamble : generator.ComputePreambles()) {
    auto it = processor->ExecuteQuery(preamble);
    if (it.Next()) {
      return base::ErrStatus(
          "Preamble query returned results. Preambles must not return. Only "
          "the last statement of the `sql` field can return results.");
    }
    PERFETTO_CHECK(!it.Next());
    RETURN_IF_ERROR(it.Status());
  }

  for (const auto& module : generator.ComputeReferencedModules()) {
    auto it = processor->ExecuteQuery("INCLUDE PERFETTO MODULE " + module);
    PERFETTO_CHECK(!it.Next());
    RETURN_IF_ERROR(it.Status());
  }

  auto queries = generator.referenced_queries();
  base::Status status = CreateQueriesAndComputeMetrics(
      processor, pool, queries, metrics, metadata_sql, output, output_spec);

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
