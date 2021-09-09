SELECT ts, name FROM ancestor_slice_by_stack((
  SELECT stack_id FROM slice
  WHERE name = 'event_depth_2'
  LIMIT 1
));
