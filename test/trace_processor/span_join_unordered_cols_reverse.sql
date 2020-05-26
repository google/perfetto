create table t1(
  a1 STRING,
  a2 BIG INT,
  dur BIG INT,
  a3 BIG INT,
  ts BIG INT PRIMARY KEY
) without rowid;

INSERT INTO t1(a1, a2, dur, a3, ts)
VALUES
("A", 1, 10, 100, 0),
("B", 2, 90, 101, 10),
("C", 3, 1, 102, 100);

create table t2(
  b1 STRING,
  ts BIG INT,
  b2 BIG INT,
  part BIG INT,
  dur BIG INT,
  b3 BIG INT,
  PRIMARY KEY (part, ts)
) without rowid;

INSERT INTO t2(b1, ts, b2, part, dur, b3)
VALUES
("A", 10, 10, 0, 90, 100),
("B", 100, 90, 0, 10, 200),
("C", 110, 1, 0, 5, 300),
("A", 5, 10, 1, 45, 100),
("B", 50, 90, 1, 40, 200),
("C", 90, 1, 1, 100, 300);

create virtual table sp using span_join(t2 PARTITIONED part, t1);

select ts,dur,part,b1,b2,b3,a1,a2,a3 from sp;
