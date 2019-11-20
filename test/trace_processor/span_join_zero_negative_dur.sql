create table t1(
  ts BIG INT,
  dur BIG INT,
  part BIG INT,
  PRIMARY KEY (part, ts, dur)
) without rowid;

INSERT INTO t1(ts, dur, part)
VALUES
(1, 0, 0),
(5, -1, 0),
(2, 0, 1);

create table t2(
  ts BIG INT,
  dur BIG INT,
  part BIG INT,
  PRIMARY KEY (part, ts, dur)
) without rowid;

INSERT INTO t2(ts, dur, part)
VALUES
(1, 2, 0),
(5, 0, 0),
(1, 1, 1);

create virtual table sp using span_outer_join(t1 PARTITIONED part, t2 PARTITIONED part);

select ts,dur,part from sp;
