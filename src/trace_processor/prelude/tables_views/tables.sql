CREATE TABLE perfetto_tables(name STRING);

CREATE TABLE trace_bounds AS
SELECT 0 AS start_ts, 0 AS end_ts;

CREATE TABLE power_profile(
  device STRING,
  cpu INT,
  cluster INT,
  freq INT,
  power DOUBLE,
  UNIQUE(device, cpu, cluster, freq)
);

CREATE TABLE trace_metrics(name STRING);

CREATE TABLE debug_slices(
  id BIGINT,
  name STRING,
  ts BIGINT,
  dur BIGINT,
  depth BIGINT
);

CREATE VIRTUAL TABLE window USING window();
