--
-- Copyright 2021 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--
-- This metric takes each flow event in a InputLatency::GestureScrollUpdate and
-- and computes the time from the ancestor_end of the current flow to the
-- ancestor_ts of the next flow event. This is a reasonable approximation of the
-- time we waited for the next step in the critical flow to start.

-- Provides the scroll_flow_event table which gives us all the flow events with
-- associated TouchMove events we care about and labels them janky or not.
SELECT RUN_METRIC('chrome/gesture_flow_event_queuing_delay.sql',
    'prefix', 'touch',
    'id_field', 'touch_id');
