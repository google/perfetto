--
-- Copyright 2019 The Android Open Source Project
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

-- Create all the views used to for LMK related stuff.
CREATE TABLE last_oom_adj(upid BIG INT PRIMARY KEY, ts BIG INT, score INT);

INSERT INTO last_oom_adj
SELECT upid, ts, score
FROM (
  SELECT ref AS upid,
         ts,
         CAST(value AS INT) AS score,
         row_number() OVER (PARTITION BY counter_id ORDER BY ts DESC) AS rank
  FROM counter_definitions JOIN counter_values USING(counter_id)
  WHERE name = 'oom_score_adj'
  AND ref_type = 'upid')
WHERE rank = 1;

CREATE VIEW lmk_events AS
SELECT ref AS upid
FROM instants
WHERE name = 'mem.lmk' AND ref_type = 'upid';

CREATE VIEW lmk_by_score AS
SELECT process.name, last_oom_adj.score
FROM lmk_events
LEFT JOIN process ON lmk_events.upid = process.upid
LEFT JOIN last_oom_adj ON lmk_events.upid = last_oom_adj.upid
ORDER BY lmk_events.upid;
