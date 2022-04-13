SELECT
  t.name,
  c.ts,
  c.value,
  EXTRACT_ARG(c.arg_set_id, 'ec_num') AS ec_num,
  EXTRACT_ARG(c.arg_set_id, 'ec_delta') AS ec_delta,
  EXTRACT_ARG(c.arg_set_id, 'sample_ts') AS sample_ts
FROM counter c
JOIN track t
  ON c.track_id = t.id
WHERE t.name == 'cros_ec.cros_ec_sensorhub_data.0';
