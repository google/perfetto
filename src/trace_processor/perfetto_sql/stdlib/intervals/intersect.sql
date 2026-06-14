--
-- Copyright 2024 The Android Open Source Project
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

-- The N-ary `_interval_intersect!` / `_ii_subquery!` / `_auto_id` machinery is
-- DELETED: absorbed by the `INTERVAL INTERSECTION OF` / `INTERVAL UNION OF`
-- operators (co-fragmenting sources with nominal payload).
--
-- The single-window clip variant is NOT an operator (a mid-pipe clip to a
-- caller-supplied scalar window) and survives as a pipeline-valued macro.

-- Clips `rel` to the half-open window `[ts, ts + dur)`: keeps every interval that
-- overlaps the window, with its bounds clamped to the window and all payload
-- columns carried through. Invoke as a source, e.g.
--   _interval_intersect_single!($ts, $dur, cpu_freq) |> AGGREGATE SUM(dur) ...
CREATE PERFETTO MACRO _interval_intersect_single(
  ts Expr,
  dur Expr,
  rel TableOrSubQuery
)
RETURNS Pipeline AS (
  FROM $rel
  |> WHERE ts < $ts + $dur AND ts + dur > $ts
  |> EXTEND MAX(ts, $ts) AS _clip_ts, MIN(ts + dur, $ts + $dur) AS _clip_end
  |> SET ts = _clip_ts
  |> SET dur = _clip_end - _clip_ts
  |> DROP _clip_ts, _clip_end
);
