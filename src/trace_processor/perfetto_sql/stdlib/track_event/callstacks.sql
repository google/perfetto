--
-- Copyright 2025 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE slices.slices;

-- Weighted callstacks from track events. Contains the weights (metric values)
-- associated with callstacks captured at track event slices.
--
-- This data comes from the `callstack_metrics` field in TrackEvent protos
-- (see protos/perfetto/trace/track_event/track_event.proto):
--   - TrackEvent.callstack_metrics: repeated CallstackMetric
--   - CallstackMetric.value: the numeric weight/metric value
--   - CallstackMetric.metric_name: inline string name (gets interned during parsing)
--   - CallstackMetric.metric_spec_iid: reference to InternedCallstackMetricSpec
--
-- Metric specifications can be interned in InternedData
-- (see protos/perfetto/trace/interned_data/interned_data.proto):
--   - InternedData.callstack_metric_specs: repeated InternedCallstackMetricSpec
--   - InternedCallstackMetricSpec.name: metric name (e.g., "CPU Time")
--   - InternedCallstackMetricSpec.description: optional description
--   - InternedCallstackMetricSpec.unit: optional unit type
--
-- This enables weighted flamegraph analysis where callstacks can be aggregated by
-- metrics like CPU time, memory usage, or any custom weight rather than just
-- sample counts.
--
-- When no metrics are specified for an event, a default "Samples" metric with
-- value 1 is automatically added.
CREATE PERFETTO VIEW track_event_weighted_callstacks (
  -- Unique identifier for this row.
  id LONG,
  -- The slice (track event) this weighted callstack is associated with.
  slice_id JOINID(slice.id),
  -- Whether this weight is from the slice's end event (1) or begin event (0).
  -- Part of the composite key with slice_id. Most weights come from begin events.
  is_end BOOL,
  -- The callstack this weight applies to.
  callsite_id JOINID(stack_profile_callsite.id),
  -- Name of the metric/weight (e.g., "CPU Time", "Memory Allocated", "Samples").
  metric_name STRING,
  -- The numeric weight value for this metric.
  value DOUBLE
) AS
SELECT
  m.id,
  m.slice_id,
  m.is_end,
  m.callsite_id,
  spec.name AS metric_name,
  m.value
FROM __intrinsic_track_event_callstack_metric AS m
JOIN __intrinsic_track_event_callstack_metric_spec AS spec
  ON m.metric_spec_id = spec.id;
