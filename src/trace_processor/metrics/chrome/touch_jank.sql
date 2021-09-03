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
-- A collection of metrics related to TouchMove events.
--
-- We define a TouchMove to be janky if comparing forwards or backwards
-- (ignoring coalesced updates) a given TouchMove exceeds the duration of its
-- predecessor or successor by 50% of a vsync interval (defaulted to 60 FPS).
--
-- WARNING: This metric should not be used as a source of truth. It is under
--          active development and the values & meaning might change without
--          notice.

SELECT RUN_METRIC(
    'chrome/gesture_jank.sql',
    'prefix', 'touch',
    'gesture_start', 'TouchStart',
    'gesture_update', 'TouchMove',
    'gesture_end', 'TouchEnd',
    'id_field', 'touch_id',
    'proto_name', 'TouchJank');
