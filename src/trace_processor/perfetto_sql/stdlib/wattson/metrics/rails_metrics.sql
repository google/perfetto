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
-- Wattson rail aggregation views for various time windows.
-- ========================================================

-- Wattson rail aggregation for Marker window
CREATE PERFETTO VIEW _wattson_rails_markers AS
SELECT
  *
FROM _wattson_rail_build_flat_view!(_wattson_window_markers);

-- Wattson rail aggregation for Full Trace
CREATE PERFETTO VIEW _wattson_rails_trace AS
SELECT
  *
FROM _wattson_rail_build_flat_view!(_wattson_window_trace);

-- Wattson rail aggregation for Startup
CREATE PERFETTO VIEW _wattson_rails_app_startup AS
SELECT
  *
FROM _wattson_rail_build_flat_view!(_wattson_window_app_startup);

-- Wattson rail aggregation for CUJ
CREATE PERFETTO VIEW _wattson_rails_atrace_apps AS
SELECT
  *
FROM _wattson_rail_build_flat_view!(_wattson_window_atrace_apps);
