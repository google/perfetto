create table t1(
  ts BIG INT,
  dur BIG INT,
  a BIG INT,
  PRIMARY KEY (ts)
) without rowid;

create table t2(
  ts BIG INT,
  dur BIG INT,
  b BIG INT,
  PRIMARY KEY (ts)
) without rowid;

-- Then insert some rows into t1 in part 1, 3, 4 and 5.
INSERT INTO t1(ts, dur, a)
VALUES
(100, 400, 111),
(500, 50, 222),
(600, 100, 333),
(900, 100, 444);

-- Insert a row into t2 which should be split up by t1's first row.
INSERT INTO t2(ts, dur, b) VALUES (50, 200, 111);

-- Insert a row into t2 should should be completely covered by t1's first row.
INSERT INTO t2(ts, dur, b) VALUES (300, 100, 222);

-- Insert a row into t2 which should span between t1's first and second rows.
INSERT INTO t2(ts, dur, b) VALUES (400, 250, 333);

create virtual table sp using span_left_join(t1, t2);

select * from sp;
