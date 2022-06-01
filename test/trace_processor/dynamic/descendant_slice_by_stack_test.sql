SELECT ts, name FROM descendant_slice_by_stack((
  SELECT stack_id FROM slice
  WHERE name = 'event_depth_0'
  LIMIT 1
));

