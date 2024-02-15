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
