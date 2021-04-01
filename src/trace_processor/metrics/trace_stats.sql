--
-- Copyright 2021 The Android Open Source Project
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

DROP VIEW IF EXISTS trace_stats_output;
CREATE VIEW trace_stats_output AS
SELECT TraceStats(
  'stat', (
    SELECT RepeatedField(TraceStats_Stat(
      'name', name,
      'idx', idx,
      'count', value,
      -- TraceStats.Source enum:
      'source', CASE source
        WHEN 'trace' THEN 1
        WHEN 'analysis' THEN 2
      END,
      -- TraceStats.Severity enum:
      'severity', CASE severity
        WHEN 'info' THEN 1
        WHEN 'data_loss' THEN 2
        WHEN 'error' THEN 3
      END
    ))
    FROM stats ORDER BY name ASC
  )
);
