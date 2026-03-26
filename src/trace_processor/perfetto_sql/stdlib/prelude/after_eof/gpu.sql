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

-- @module prelude.after_eof.gpu

-- Contains information about the GPUs on the device this trace was taken on.
CREATE PERFETTO VIEW gpu (
  -- Unique identifier for this GPU. Identical to |ugpu|, prefer using |ugpu|
  -- instead.
  id ID,
  -- Unique identifier for this GPU. Isn't equal to |gpu| for remote machines
  -- and is equal to |gpu| for the host machine.
  ugpu ID,
  -- The 0-based GPU index.
  gpu LONG,
  -- Machine identifier
  machine_id JOINID(machine.id)
) AS
SELECT
  id,
  id AS ugpu,
  gpu,
  machine_id
FROM __intrinsic_gpu
WHERE
  gpu IS NOT NULL;
