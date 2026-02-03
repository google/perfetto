--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE wattson.metrics.aggregation;

INCLUDE PERFETTO MODULE wattson.metrics.windows;

-- ========================================================
-- Wattson thread aggregation views for various time windows.
-- ========================================================

-- Wattson thread aggregation for Marker window
CREATE PERFETTO VIEW _wattson_threads_marker AS
SELECT
  *
FROM _wattson_threads_build_flat_view!(_wattson_window_marker);

-- Wattson thread aggregation for Full Trace
CREATE PERFETTO VIEW _wattson_threads_trace AS
SELECT
  *
FROM _wattson_threads_build_flat_view!(_wattson_window_trace);

-- Wattson thread aggregation for Startup
CREATE PERFETTO VIEW _wattson_threads_startup AS
SELECT
  *
FROM _wattson_threads_build_flat_view!(_wattson_window_startup);

-- Wattson thread aggregation for CUJ
CREATE PERFETTO VIEW _wattson_threads_cuj AS
SELECT
  *
FROM _wattson_threads_build_flat_view!(_wattson_window_cuj);
