SELECT
  id,
  depth,
  name,
  count,
  cumulative_count,
  size,
  cumulative_size,
  parent_id
FROM experimental_flamegraph
where upid = (select max(upid) from heap_graph_object)
  and profile_type = 'graph'
  and ts = (select max(graph_sample_ts) from heap_graph_object)
  and focus_str = 'left'
LIMIT 10
