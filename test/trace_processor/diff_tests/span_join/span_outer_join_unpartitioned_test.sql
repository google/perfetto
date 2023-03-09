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
CREATE TABLE t1(
  ts BIGINT,
  dur BIGINT,
  a BIGINT,
  PRIMARY KEY (ts)
) WITHOUT ROWID;

CREATE TABLE t2(
  ts BIGINT,
  dur BIGINT,
  b BIGINT,
  PRIMARY KEY (ts)
) WITHOUT ROWID;

-- Add some rows to t1.
INSERT INTO t1(ts, dur, a)
VALUES
(100, 400, 1),
(500, 50, 2),
(600, 100, 3);

-- Add some rows to t2.
INSERT INTO t2(ts, dur, b)
VALUES
(50, 100, 4),
(550, 50, 5),
(600, 50, 6),
(900, 500, 7);

CREATE VIRTUAL TABLE sp USING span_outer_join(t1, t2);

SELECT * FROM sp;
