-- For each heap graph dump (upid, ts), builds a flamegraph and outputs:
-- * total_objects_size: the sum of the size (native + java) of all the
--   reachable objects
-- * total_flamegraph_size: the sum of the cumulative size of the roots in the
--   flamegraph
-- If the flamegraph has been built correctly, the numbers should match.
SELECT
  obj.upid AS upid,
  obj.graph_sample_ts AS ts,
  SUM(obj.self_size + obj.native_size) AS total_objects_size,
  (
    SELECT SUM(cumulative_size)
    FROM experimental_flamegraph
    WHERE experimental_flamegraph.upid = obj.upid
      AND experimental_flamegraph.ts = obj.graph_sample_ts
      AND profile_type = 'graph'
      AND depth = 0 -- only the roots
  ) AS total_flamegraph_size
FROM
  heap_graph_object AS obj
WHERE
  obj.reachable != 0
GROUP BY obj.upid, obj.graph_sample_ts
