--
-- Copyright 2022 The Android Open Source Project
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
-- Those are helper functions used in computing jank metrics

-- This function takes timestamps of two consecutive frames and determines if
-- its janky by a delay of more than 0.5 of a frame  in order to make sure that
-- the comparison does not filter out ratios that are precisely 0.5, which can
-- fall a little above or below exact value due to inherent inaccuracy of operations with
-- floating-point numbers. Value 1e-9 have been chosen as follows: the ratio has
-- nanoseconds in numerator and VSync interval in denominator. Assuming refresh
-- rate more than 1 FPS (and therefore VSync interval less than a second), this
-- ratio should increase with increments more than minimal value in numerator
-- (1ns) divided by maximum value in denominator, giving 1e-9.

SELECT CREATE_FUNCTION(
  -- Function : function takes scroll ids of frames to verify it's from
  -- the same scroll, and makes sure the frame ts occured within the scroll
  -- timestamp of the neighbour and computes whether the frame was janky or not.
  'IsJankyFrame(cur_id LONG,next_id LONG,neighbour_ts LONG,' ||
  'cur_begin_ts LONG,cur_gesture_end LONG,cur_frame_exact FLOAT,' ||
  'neighbour_frame_exact FLOAT)',
  -- Returns true if the frame was janky, false otherwise
  'BOOL',
  'SELECT
    CASE WHEN
      $cur_id != $next_id OR
      $neighbour_ts IS NULL OR
      $neighbour_ts < $cur_begin_ts OR
      $neighbour_ts > $cur_gesture_end THEN
        FALSE ELSE
        $cur_frame_exact > $neighbour_frame_exact + 0.5 + 1e-9
    END'
);
