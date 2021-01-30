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

SELECT RUN_METRIC(
  'android/composition_layers.sql',
  'track_name', 'HWComposer: Total Layer',
  'output', 'total_layers'
);

SELECT RUN_METRIC(
  'android/composition_layers.sql',
  'track_name', 'HWComposer: DPU Layer',
  'output', 'dpu_layers'
);

SELECT RUN_METRIC(
  'android/composition_layers.sql',
  'track_name', 'HWComposer: GPU Layer',
  'output', 'gpu_layers'
);

SELECT RUN_METRIC(
  'android/composition_layers.sql',
  'track_name', 'HWComposer: Cached Layer',
  'output', 'cached_layers'
);

DROP VIEW IF EXISTS android_hwcomposer_output;
CREATE VIEW android_hwcomposer_output AS
SELECT AndroidHwcomposerMetrics(
    'composition_total_layers', (SELECT AVG(value)
                            FROM total_layers),
    'composition_dpu_layers', (SELECT AVG(value)
                            FROM dpu_layers),
    'composition_gpu_layers', (SELECT AVG(value)
                            FROM gpu_layers),
    'composition_dpu_cached_layers', (SELECT AVG(value)
                            FROM cached_layers)
);
