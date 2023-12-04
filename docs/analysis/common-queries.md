# PerfettoSQL Common Queries

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

## Computing scheduling time by woken threads
A given thread might cause other threads to wake up i.e. because work was
scheduled on them. For a given thread, the amount of time threads it
woke up ran for can be a good proxy to understand how much work is being
spawned.

To compute this, the following query can be used:
```
SELECT
  SUM((
    SELECT dur FROM sched
    WHERE
      sched.ts > wakee_runnable.ts AND
      wakee_runnable.utid = wakee_runnable.utid
    ORDER BY ts
    LIMIT 1
  )) AS scheduled_dur
FROM thread AS waker
JOIN thread_state AS wakee_runnable ON waker.utid = wakee_runnable.waker_utid
WHERE waker.name = <your waker thread name here>
```

To do this for all the threads in the trace simultaenously:
```
SELECT
  waker_process.name AS process_name,
  waker.name AS thread_name,
  SUM((
    SELECT dur FROM sched
    WHERE
      sched.ts > wakee_runnable.ts AND
      sched.utid = wakee_runnable.utid
    ORDER BY ts
    LIMIT 1
  )) AS scheduled_dur
FROM thread AS waker
JOIN process AS waker_process USING (upid)
JOIN thread_state AS wakee_runnable ON waker.utid = wakee_runnable.waker_utid
WHERE waker.utid != 0
GROUP BY 1, 2
ORDER BY 3 desc
```
