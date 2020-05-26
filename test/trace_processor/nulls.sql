CREATE TABLE null_test (
  primary_key INTEGER PRIMARY KEY,
  int_nulls INTEGER,
  string_nulls STRING,
  double_nulls DOUBLE,
  start_int_nulls INTEGER,
  start_string_nulls STRING,
  start_double_nulls DOUBLE,
  all_nulls INTEGER
);

INSERT INTO null_test(
  int_nulls,
  string_nulls,
  double_nulls,
  start_int_nulls,
  start_string_nulls,
  start_double_nulls
)
VALUES
(1,     "test",   2.0,  NULL, NULL,   NULL),
(2,     NULL,     NULL, NULL, "test", NULL),
(1,     "other",  NULL, NULL, NULL,   NULL),
(4,     NULL,     NULL, NULL, NULL,   1.0),
(NULL,  "test",   1.0,  1,    NULL,   NULL)

SELECT * from null_test;
