CREATE VIEW counters AS 
SELECT *
FROM counter v 
JOIN counter_track t ON v.track_id = t.id 
ORDER BY ts;

CREATE VIEW slice AS 
SELECT
  *, 
  category AS cat, 
  id AS slice_id 
FROM internal_slice;

CREATE VIEW instant AS 
SELECT ts, track_id, name, arg_set_id 
FROM slice 
WHERE dur = 0;

CREATE VIEW sched AS 
SELECT 
  *,
  ts + dur as ts_end
FROM sched_slice;

CREATE VIEW slices AS 
SELECT * FROM slice;

CREATE VIEW thread AS 
SELECT 
  id as utid,
  *
FROM internal_thread;

CREATE VIEW process AS 
SELECT 
  id as upid,
  * 
FROM internal_process;

-- This should be kept in sync with GlobalArgsTracker::AddArgSet.
CREATE VIEW args AS 
SELECT 
  *,
  CASE value_type
    WHEN 'int' THEN CAST(int_value AS text)
    WHEN 'uint' THEN CAST(int_value AS text)
    WHEN 'string' THEN string_value
    WHEN 'real' THEN CAST(real_value AS text)
    WHEN 'pointer' THEN printf('0x%x', int_value)
    WHEN 'bool' THEN (
      CASE WHEN int_value <> 0 THEN 'true'
      ELSE 'false' END)
    WHEN 'json' THEN string_value
  ELSE NULL END AS display_value
FROM internal_args;
