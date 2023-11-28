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
CREATE TABLE t1(
  a1 STRING,
  a2 BIGINT,
  dur BIGINT,
  a3 BIGINT,
  ts BIGINT PRIMARY KEY
) WITHOUT ROWID;

INSERT INTO t1(a1, a2, dur, a3, ts)
VALUES
("A", 1, 10, 100, 0),
("B", 2, 90, 101, 10),
("C", 3, 1, 102, 100);

CREATE TABLE t2(
  b1 STRING,
  ts BIGINT,
  b2 BIGINT,
  part BIGINT,
  dur BIGINT,
  b3 BIGINT,
  PRIMARY KEY (part, ts)
) WITHOUT ROWID;

INSERT INTO t2(b1, ts, b2, part, dur, b3)
VALUES
("A", 10, 10, 0, 90, 100),
("B", 100, 90, 0, 10, 200),
("C", 110, 1, 0, 5, 300),
("A", 5, 10, 1, 45, 100),
("B", 50, 90, 1, 40, 200),
("C", 90, 1, 1, 100, 300);

CREATE VIRTUAL TABLE sp USING span_join(t1, t2 PARTITIONED part);

SELECT ts, dur, part, b1, b2, b3, a1, a2, a3 FROM sp;
