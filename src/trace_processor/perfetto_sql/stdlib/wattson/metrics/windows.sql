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

INCLUDE PERFETTO MODULE android.cujs.base;

INCLUDE PERFETTO MODULE android.startup.startups;

INCLUDE PERFETTO MODULE wattson.utils;

-- ========================================================
-- Window definitions for Wattson metric analysis.
-- ========================================================

-- Standardized window for Wattson markers
CREATE PERFETTO VIEW _wattson_window_markers AS
SELECT
  ts,
  dur,
  1 AS period_id
FROM _wattson_markers_window;

-- Standardized window for the full trace duration
CREATE PERFETTO VIEW _wattson_window_trace AS
SELECT
  trace_start() AS ts,
  trace_dur() AS dur,
  1 AS period_id;

-- Standardized window for Android app startups
CREATE PERFETTO VIEW _wattson_window_app_startup AS
SELECT
  ts,
  dur,
  startup_id AS period_id
FROM android_startups;

-- Standardized window for Android CUJs
CREATE PERFETTO VIEW _wattson_window_atrace_apps AS
SELECT
  ts,
  dur,
  cuj_id AS period_id
FROM android_jank_cuj;
