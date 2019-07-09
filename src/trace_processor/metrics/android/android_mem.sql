--
-- Copyright 2019 The Android Open Source Project
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
--

SELECT RUN_METRIC('android/process_mem.sql');

SELECT RUN_METRIC('android/span_view_stats.sql', 'table_name', 'anon_rss');

SELECT RUN_METRIC('android/span_view_stats.sql', 'table_name', 'file_rss');

SELECT RUN_METRIC('android/span_view_stats.sql', 'table_name', 'swap');

SELECT RUN_METRIC('android/span_view_stats.sql', 'table_name', 'anon_and_swap');

SELECT RUN_METRIC('android/mem_stats_priority_breakdown.sql', 'table_name', 'anon_rss');

SELECT RUN_METRIC('android/mem_stats_priority_breakdown.sql', 'table_name', 'file_rss');

SELECT RUN_METRIC('android/mem_stats_priority_breakdown.sql', 'table_name', 'swap');

SELECT RUN_METRIC('android/mem_stats_priority_breakdown.sql', 'table_name', 'anon_and_swap');

CREATE VIEW process_priority_view AS
SELECT
  process_name,
  AndroidMemoryMetric_PriorityBreakdown(
    'priority', priority,
    'counters', AndroidMemoryMetric_ProcessMemoryCounters(
      'anon_rss', AndroidMemoryMetric_Counter(
        'min', anon_rss_by_priority_stats.min_value,
        'max', anon_rss_by_priority_stats.max_value,
        'avg', anon_rss_by_priority_stats.avg_value
      ),
      'file_rss', AndroidMemoryMetric_Counter(
        'min', file_rss_by_priority_stats.min_value,
        'max', file_rss_by_priority_stats.max_value,
        'avg', file_rss_by_priority_stats.avg_value
      ),
      'swap', AndroidMemoryMetric_Counter(
        'min', swap_by_priority_stats.min_value,
        'max', swap_by_priority_stats.max_value,
        'avg', swap_by_priority_stats.avg_value
      ),
      'anon_and_swap', AndroidMemoryMetric_Counter(
        'min', anon_and_swap_by_priority_stats.min_value,
        'max', anon_and_swap_by_priority_stats.max_value,
        'avg', anon_and_swap_by_priority_stats.avg_value
      )
    )
  ) AS priority_breakdown_proto
FROM anon_rss_by_priority_stats
JOIN file_rss_by_priority_stats USING (process_name, priority)
JOIN swap_by_priority_stats USING (process_name, priority)
JOIN anon_and_swap_by_priority_stats USING (process_name, priority);

CREATE VIEW process_metrics_view AS
SELECT
  AndroidMemoryMetric_ProcessMetrics(
    'process_name', process_name,
    'total_counters', AndroidMemoryMetric_ProcessMemoryCounters(
      'anon_rss', AndroidMemoryMetric_Counter(
        'min', anon_rss_stats.min_value,
        'max', anon_rss_stats.max_value,
        'avg', anon_rss_stats.avg_value
      ),
      'file_rss', AndroidMemoryMetric_Counter(
        'min', file_rss_stats.min_value,
        'max', file_rss_stats.max_value,
        'avg', file_rss_stats.avg_value
      ),
      'swap', AndroidMemoryMetric_Counter(
        'min', swap_stats.min_value,
        'max', swap_stats.max_value,
        'avg', swap_stats.avg_value
      ),
      'anon_and_swap', AndroidMemoryMetric_Counter(
        'min', anon_and_swap_stats.min_value,
        'max', anon_and_swap_stats.max_value,
        'avg', anon_and_swap_stats.avg_value
      )
    ),
    'priority_breakdown', (
      SELECT RepeatedField(priority_breakdown_proto)
      FROM process_priority_view AS ppv
      WHERE anon_rss_stats.process_name = ppv.process_name
    )
  ) AS metric
FROM
  anon_rss_stats
  JOIN file_rss_stats USING (process_name)
  JOIN swap_stats USING (process_name)
  JOIN anon_and_swap_stats USING (process_name);

CREATE VIEW android_mem_output AS
SELECT
  AndroidMemoryMetric(
    'process_metrics',
    (SELECT RepeatedField(metric) FROM process_metrics_view)
  );
