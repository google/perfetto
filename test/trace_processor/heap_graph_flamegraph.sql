SELECT
  id,
  depth,
  name,
  map_name,
  count,
  cumulative_count,
  size,
  cumulative_size,
  parent_id
FROM experimental_flamegraph(601908408518618, 1, 'graph')
LIMIT 10
