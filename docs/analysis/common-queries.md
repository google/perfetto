# Common queries

This page acts as a reference guide for queries which often appear when
performing ad-hoc analysis.

## Computing CPU time for slices
If collecting traces which including scheduling information (i.e. from ftrace)
as well as userspace slices (i.e. from atrace), the actual time spent running
on a CPU for each userspace slice can be computed: this is commonly known as
the "CPU time" for a slice.

Firstly, setup the views to simplify subsequent queries:
```
DROP VIEW IF EXISTS slice_with_utid;
CREATE VIEW slice_with_utid AS
SELECT
  ts,
  dur,
  slice.name as slice_name,
  slice.id as slice_id, utid,
  thread.name as thread_name
FROM slice
JOIN thread_track ON thread_track.id = slice.track_id
JOIN thread USING (utid);

DROP TABLE IF EXISTS slice_thread_state_breakdown;
CREATE VIRTUAL TABLE slice_thread_state_breakdown
USING SPAN_LEFT_JOIN(
  slice_with_utid PARTITIONED utid,
  thread_state PARTITIONED utid
);
```

Then, to compute the CPU time for all slices in the trace:
```
SELECT slice_id, slice_name, SUM(dur) AS cpu_time
FROM slice_thread_state_breakdown
WHERE state = 'Running'
GROUP BY slice_id;
```

You can also compute CPU time for a specific slice:
```
SELECT slice_name, SUM(dur) AS cpu_time
FROM slice_thread_state_breakdown
WHERE slice_id = <your slice id> AND state = 'Running';
```

These queries can be varied easily to compute other similar metrics.
For example to get the time spent "runnable" and in "uninterruptible sleep":
```
SELECT
  slice_id,
  slice_name,
  SUM(CASE state = 'R' THEN dur ELSE 0 END) AS runnable_time,
  SUM(CASE state = 'D' THEN dur ELSE 0 END) AS uninterruptible_time
FROM slice_thread_state_breakdown
GROUP BY slice_id;
```
