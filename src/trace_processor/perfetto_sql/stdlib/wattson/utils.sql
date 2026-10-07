--
-- Copyright 2025 The Android Open Source Project
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

-- Creates slice that defines the user inserted Wattson markers. This table will
-- provide the proper Wattson markers slice if user inserts only one pair of
-- Wattson markers, which is the pre-defined agreement.
CREATE PERFETTO TABLE _wattson_markers_window AS
WITH
  markers AS (
    SELECT
      min(ts) FILTER (WHERE name = 'wattson_start') AS start,
      max(ts) FILTER (WHERE name = 'wattson_stop') AS stop
    FROM slice
    WHERE
      name IN ('wattson_start', 'wattson_stop')
  )
SELECT start AS ts, stop - start AS dur, 'Markers window' AS name
FROM markers
WHERE
  start IS NOT NULL;

-- Helper macro for using Perfetto table with interval intersect
CREATE PERFETTO MACRO _ii_subquery(tab TableOrSubquery)
RETURNS TableOrSubquery
AS (
  SELECT
    _auto_id AS id,
    *
  FROM $tab
);

-- DSU dependency policy
CREATE PERFETTO MACRO _dsu_dep()
RETURNS Expr
AS 255;

-- Constructs an 8-bit mask from 8 boolean expressions
CREATE PERFETTO MACRO _bitmask8(
  b0 Expr,
  b1 Expr,
  b2 Expr,
  b3 Expr,
  b4 Expr,
  b5 Expr,
  b6 Expr,
  b7 Expr
)
RETURNS Expr
AS (
  (
    (
      $b0 != 0
    ) | (
      (
        $b1 != 0
      ) << 1
    ) | (
      (
        $b2 != 0
      ) << 2
    ) | (
      (
        $b3 != 0
      ) << 3
    ) | (
      (
        $b4 != 0
      ) << 4
    ) | (
      (
        $b5 != 0
      ) << 5
    ) | (
      (
        $b6 != 0
      ) << 6
    ) | (
      (
        $b7 != 0
      ) << 7
    )
  )
);

-- Extracts the bit at 'bit' index from 'mask'
CREATE PERFETTO MACRO _extract_bit(mask Expr, bit Expr)
RETURNS Expr
AS (
  (
    (
      $mask
    ) >> (
      $bit
    )
  ) & 1
);

-- Constructs an 8-bit mask based on policy matches
CREATE PERFETTO MACRO _policy_mask(
  target Expr,
  p0 Expr,
  p1 Expr,
  p2 Expr,
  p3 Expr,
  p4 Expr,
  p5 Expr,
  p6 Expr,
  p7 Expr
)
RETURNS Expr
AS (
  _bitmask8!(
    $p0 != -1 AND $p0 = $target,
    $p1 != -1 AND $p1 = $target,
    $p2 != -1 AND $p2 = $target,
    $p3 != -1 AND $p3 = $target,
    $p4 != -1 AND $p4 = $target,
    $p5 != -1 AND $p5 = $target,
    $p6 != -1 AND $p6 = $target,
    $p7 != -1 AND $p7 = $target
  )
);

-- 3-bit idle_id encoding (bits 0..2 of state_id):
--   0..2: inactive states (_deepest_idle_id, _offline_idle_id, _null_idle_id) -> bit 2 is 0
--   4..7: active idle states (_encode_idle!(idle) = idle + 5 for idle in -1..2) -> bit 2 is 1
CREATE PERFETTO MACRO _deepest_idle_id()
RETURNS Expr
AS 0;

CREATE PERFETTO MACRO _offline_idle_id()
RETURNS Expr
AS 1;

CREATE PERFETTO MACRO _null_idle_id()
RETURNS Expr
AS 2;

CREATE PERFETTO MACRO _idle_id_mask()
RETURNS Expr
AS 7;

CREATE PERFETTO MACRO _cpu_active_bit()
RETURNS Expr
AS 4;

CREATE PERFETTO MACRO _encode_idle(idle Expr)
RETURNS Expr
AS (($idle + 5) & _idle_id_mask!());

CREATE PERFETTO MACRO _is_cpu_active(state_id Expr)
RETURNS Expr
AS (($state_id) & _cpu_active_bit!());

CREATE PERFETTO MACRO _cpu_state_cpu_shift()
RETURNS Expr
AS 26;

CREATE PERFETTO MACRO _cpu_state_freq_shift()
RETURNS Expr
AS 3;

-- Encodes (cpu, freq_khz, idle_id) into a 29-bit state_id:
--   bits 26..28: cpu (0..7)
--   bits 3..25:  freq_khz (0..8388607 kHz)
--   bits 0..2:   idle_id (0=deepest, 1=offline, 2=null, 4..7=active idle)
CREATE PERFETTO MACRO _pack_cpu_state(cpu Expr, freq Expr, idle_id Expr)
RETURNS Expr
AS (
  (($cpu) << _cpu_state_cpu_shift!())
  | (($freq) << _cpu_state_freq_shift!())
  | (($idle_id) & _idle_id_mask!())
);
