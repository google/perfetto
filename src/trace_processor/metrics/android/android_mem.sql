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
