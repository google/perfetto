CREATE TABLE t1(
  ts BIGINT,
  dur BIGINT,
  part BIGINT,
  PRIMARY KEY (part, ts)
) WITHOUT ROWID;

CREATE TABLE t2(
  ts BIGINT,
  dur BIGINT,
  part BIGINT,
  PRIMARY KEY (part, ts)
) WITHOUT ROWID;

-- Insert a single row into t1.
INSERT INTO t1(ts, dur, part)
VALUES (500, 100, 10);

-- Insert a single row into t2.
INSERT INTO t2(ts, dur, part)
VALUES (500, 100, 5);

CREATE VIRTUAL TABLE sp USING span_left_join(t1 PARTITIONED part,
                                             t2 PARTITIONED part);

SELECT * FROM sp;
