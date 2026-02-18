// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {formatSQL} from './format';

// Normalize whitespace for comparison - trim lines and collapse blanks.
function norm(s: string): string {
  return s
    .trim()
    .split('\n')
    .map((l) => l.trimEnd())
    .join('\n');
}

describe('Format: INCLUDE PERFETTO MODULE', () => {
  test('formats simple include', () => {
    const input = `include perfetto module android.frames.timeline;`;
    const expected = `INCLUDE PERFETTO MODULE android.frames.timeline;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats multiple includes', () => {
    const input = `
      include perfetto module intervals.intersect;
      include perfetto module slices.with_context;
    `;
    const expected = `INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE slices.with_context;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: SELECT statements', () => {
  test('formats basic SELECT with FROM and WHERE', () => {
    const input = `select id, name from my_table where id = 1;`;
    const expected = `SELECT
  id,
  name
FROM my_table
WHERE id = 1;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT *', () => {
    const input = `select * from foo;`;
    const expected = `SELECT
  *
FROM foo;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT DISTINCT', () => {
    const input = `select distinct name from items;`;
    const expected = `SELECT DISTINCT
  name
FROM items;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT with aliases', () => {
    const input = `select a.id as aid, b.name as bname from tbl as a;`;
    const expected = `SELECT
  a.id AS aid,
  b.name AS bname
FROM tbl AS a;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT with double-quoted identifiers', () => {
    const input = `select id as "foo" from slice;`;
    const expected = `SELECT
  id AS "foo"
FROM slice;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT with GROUP BY and ORDER BY', () => {
    const input = `select name, count() as cnt from items group by name order by cnt desc;`;
    const expected = `SELECT
  name,
  count() AS cnt
FROM items
GROUP BY name
ORDER BY cnt DESC;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT with JOIN and ON', () => {
    const input = `select a.x, b.y from a join b on a.id = b.id;`;
    const expected = `SELECT
  a.x,
  b.y
FROM a
JOIN b ON a.id = b.id;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT with LEFT JOIN and USING', () => {
    const input = `select * from a left join b using (id);`;
    const expected = `SELECT
  *
FROM a
LEFT JOIN b USING (id);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT with LIMIT and OFFSET', () => {
    const input = `select id from t limit 10 offset 5;`;
    const expected = `SELECT
  id
FROM t
LIMIT 10 OFFSET 5;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('preserves parenthesized table name', () => {
    const input = `select * from (slice);`;
    const expected = `SELECT
  *
FROM (slice);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats UNION ALL', () => {
    const input = `select * from a union all select * from b;`;
    const expected = `SELECT
  *
FROM a
UNION ALL
SELECT
  *
FROM b;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SELECT with window function', () => {
    const input = `select id, lag(id) over (partition by utid order by ts) as prev_id from events;`;
    const expected = `SELECT
  id,
  lag(id) OVER (PARTITION BY utid ORDER BY ts) AS prev_id
FROM events;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats CASE expression', () => {
    const input = `select case when x = 1 then 'a' when x = 2 then 'b' else 'c' end as val from t;`;
    const expected = `SELECT
  CASE WHEN x = 1 THEN 'a' WHEN x = 2 THEN 'b' ELSE 'c' END AS val
FROM t;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats CAST expression', () => {
    const input = `select cast(x as INTEGER) from t;`;
    const expected = `SELECT
  CAST(x AS INTEGER)
FROM t;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats HAVING clause', () => {
    const input = `select name, count() as c from t group by name having c > 5;`;
    const expected = `SELECT
  name,
  count() AS c
FROM t
GROUP BY name
HAVING c > 5;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats qualified star (r.*)', () => {
    const input = `select r.* from results r;`;
    const expected = `SELECT
  r.*
FROM results AS r;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats bare table alias (FROM t r)', () => {
    const input = `select id from my_table t where t.id = 1;`;
    const expected = `SELECT
  id
FROM my_table AS t
WHERE t.id = 1;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats bare aliases in JOIN', () => {
    const input = `select a.x, b.y from tbl1 a join tbl2 b on a.id = b.id;`;
    const expected = `SELECT
  a.x,
  b.y
FROM tbl1 AS a
JOIN tbl2 AS b ON a.id = b.id;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: WITH statements', () => {
  test('formats simple CTE', () => {
    const input = `with cte as (select 1 as x) select * from cte;`;
    const expected = `WITH
  cte AS (
    SELECT
      1 AS x
  )
SELECT
  *
FROM cte;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats multiple CTEs', () => {
    const input = `with a as (select 1 as x), b as (select 2 as y) select * from a join b on a.x = b.y;`;
    const expected = `WITH
  a AS (
    SELECT
      1 AS x
  ),
  b AS (
    SELECT
      2 AS y
  )
SELECT
  *
FROM a
JOIN b ON a.x = b.y;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats MATERIALIZED CTE', () => {
    const input = `with metrics as materialized (select callsite_id, sum(value) as v from s group by callsite_id) select * from metrics;`;
    const expected = `WITH
  metrics AS MATERIALIZED (
    SELECT
      callsite_id,
      sum(value) AS v
    FROM s
    GROUP BY callsite_id
  )
SELECT
  *
FROM metrics;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: CREATE PERFETTO TABLE', () => {
  test('formats simple CREATE PERFETTO TABLE', () => {
    const input = `create perfetto table my_table as select * from source;`;
    const expected = `CREATE PERFETTO TABLE my_table AS
SELECT
  *
FROM source;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats CREATE PERFETTO TABLE with column defs', () => {
    const input = `create perfetto table my_table(name STRING, id JOINID(track.id)) as select name, id from source;`;
    const expected = `CREATE PERFETTO TABLE my_table(
  name STRING,
  id JOINID(track.id)
) AS
SELECT
  name,
  id
FROM source;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: CREATE PERFETTO VIEW', () => {
  test('formats simple CREATE PERFETTO VIEW', () => {
    const input = `create perfetto view my_view as select id, name from source;`;
    const expected = `CREATE PERFETTO VIEW my_view AS
SELECT
  id,
  name
FROM source;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats CREATE PERFETTO VIEW with column defs', () => {
    const input = `create perfetto view counter_track(id ID(track.id), name STRING, unit STRING) as select id, name, counter_unit as unit from __intrinsic_track where event_type = 'counter';`;
    const expected = `CREATE PERFETTO VIEW counter_track(
  id ID(track.id),
  name STRING,
  unit STRING
) AS
SELECT
  id,
  name,
  counter_unit AS unit
FROM __intrinsic_track
WHERE event_type = 'counter';`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: CREATE PERFETTO FUNCTION', () => {
  test('formats function returning TABLE', () => {
    const input = `create perfetto function _get_rate(event STRING) returns table(ts TIMESTAMP, dur DURATION) as select ts, dur from counter;`;
    const expected = `CREATE PERFETTO FUNCTION _get_rate(event STRING)
RETURNS TABLE(ts TIMESTAMP, dur DURATION)
AS
SELECT
  ts,
  dur
FROM counter;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats function with DELEGATES TO', () => {
    const input = `create perfetto function _tree_constraint(column STRING, op STRING, value ANY) returns ANY delegates to __intrinsic_tree_constraint;`;
    const expected = `CREATE PERFETTO FUNCTION _tree_constraint(column STRING, op STRING, value ANY)
RETURNS ANY
DELEGATES TO __intrinsic_tree_constraint;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: CREATE PERFETTO MACRO', () => {
  test('formats simple macro', () => {
    const input = `create perfetto macro my_macro(src TableOrSubquery) returns TableOrSubquery as (select * from $src);`;
    const expected = `CREATE PERFETTO MACRO my_macro(src TableOrSubquery)
RETURNS TableOrSubquery
AS (
SELECT
  *
FROM $src
);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: CREATE PERFETTO INDEX', () => {
  test('formats simple index', () => {
    const input = `create perfetto index my_idx on my_table(col1, col2);`;
    const expected = `CREATE PERFETTO INDEX my_idx
  ON my_table(col1, col2);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: CREATE VIRTUAL TABLE', () => {
  test('formats virtual table', () => {
    const input = `create virtual table _arm_l3_rates using SPAN_OUTER_JOIN(_arm_l3_miss_rate, _arm_l3_hit_rate);`;
    const expected = `CREATE VIRTUAL TABLE _arm_l3_rates USING SPAN_OUTER_JOIN(_arm_l3_miss_rate, _arm_l3_hit_rate);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: Complex queries', () => {
  test('formats WITH RECURSIVE', () => {
    const input = `with recursive tree as (select id, parent_id, 0 as depth from nodes where parent_id is null union all select n.id, n.parent_id, t.depth + 1 from nodes n join tree t on n.parent_id = t.id) select * from tree;`;
    const expected = `WITH RECURSIVE
  tree AS (
    SELECT
      id,
      parent_id,
      0 AS depth
    FROM nodes
    WHERE parent_id IS NULL
    UNION ALL
    SELECT
      n.id,
      n.parent_id,
      t.depth + 1
    FROM nodes AS n
    JOIN tree AS t ON n.parent_id = t.id
  )
SELECT
  *
FROM tree;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats ROW_NUMBER window function', () => {
    // Note: function names like row_number are not uppercased (only SQL keywords)
    const input = `select id, row_number() over (partition by category order by ts desc) as rn from events;`;
    const expected = `SELECT
  id,
  row_number() OVER (PARTITION BY category ORDER BY ts DESC) AS rn
FROM events;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats LEAD/LAG window functions', () => {
    const input = `select id, lead(ts) over (partition by utid order by id) - ts as next_dur from thread_state;`;
    const expected = `SELECT
  id,
  lead(ts) OVER (PARTITION BY utid ORDER BY id) - ts AS next_dur
FROM thread_state;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats multiple JOINs with USING', () => {
    const input = `select s.name, t.name as thread_name, p.name as process_name from slice s join thread_track tt on s.track_id = tt.id join thread t using (utid) join process p using (upid);`;
    const expected = `SELECT
  s.name,
  t.name AS thread_name,
  p.name AS process_name
FROM slice AS s
JOIN thread_track AS tt ON s.track_id = tt.id
JOIN thread AS t USING (utid)
JOIN process AS p USING (upid);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats COALESCE and IFNULL', () => {
    const input = `select coalesce(name, 'unknown') as name, ifnull(dur, 0) as dur from slice;`;
    const expected = `SELECT
  coalesce(name, 'unknown') AS name,
  ifnull(dur, 0) AS dur
FROM slice;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats subquery in WHERE clause', () => {
    // Note: subqueries in expressions are kept inline (not expanded)
    const input = `select * from slice where track_id in (select id from track where name like 'binder%');`;
    const expected = `SELECT
  *
FROM slice
WHERE track_id IN (SELECT id FROM track WHERE name LIKE 'binder%');`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats complex CASE with multiple WHEN clauses', () => {
    const input = `select id, case when state = 'R' then 'running' when state = 'S' then 'sleeping' when state = 'D' then 'blocked' else 'other' end as state_name from thread_state;`;
    const expected = `SELECT
  id,
  CASE WHEN state = 'R' THEN 'running' WHEN state = 'S' THEN 'sleeping' WHEN state = 'D' THEN 'blocked' ELSE 'other' END AS state_name
FROM thread_state;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats MAX/MIN in subquery', () => {
    // Note: scalar subqueries in SELECT columns are kept inline
    const input = `select (select max(id) + 1 from heap_graph_object) as new_id;`;
    const expected = `SELECT
  (SELECT max(id) + 1 FROM heap_graph_object) AS new_id;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats EXISTS subquery', () => {
    // Note: EXISTS subqueries are kept inline
    const input = `select * from slice where exists (select 1 from thread_track where thread_track.id = slice.track_id);`;
    const expected = `SELECT
  *
FROM slice
WHERE EXISTS (SELECT 1 FROM thread_track WHERE thread_track.id = slice.track_id);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats PRINTF function', () => {
    const input = `select printf('%010d', local_rank) as sort_key from nodes;`;
    const expected = `SELECT
  printf('%010d', local_rank) AS sort_key
FROM nodes;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats GLOB pattern matching', () => {
    const input = `select * from slice where name glob 'binder*';`;
    const expected = `SELECT
  *
FROM slice
WHERE name GLOB 'binder*';`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats EXCEPT with explicit alias', () => {
    // Note: bare aliases conflict with EXCEPT/INTERSECT keywords after table names.
    // Use explicit AS to avoid ambiguity.
    const input = `select id from table_a as a except select id from table_b;`;
    const expected = `SELECT
  id
FROM table_a AS a
EXCEPT
SELECT
  id
FROM table_b;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats INTERSECT with explicit alias', () => {
    const input = `select id from table_a as a intersect select id from table_b;`;
    const expected = `SELECT
  id
FROM table_a AS a
INTERSECT
SELECT
  id
FROM table_b;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats IIF function', () => {
    const input = `select iif(name is null, 'unknown', name) as display_name from slice;`;
    const expected = `SELECT
  iif(name IS NULL, 'unknown', name) AS display_name
FROM slice;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats BETWEEN in JOIN condition', () => {
    const input = `select * from events e join ranges r on e.ts between r.start_ts and r.end_ts;`;
    const expected = `SELECT
  *
FROM events AS e
JOIN ranges AS r ON e.ts BETWEEN r.start_ts AND r.end_ts;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats string concatenation', () => {
    const input = `select path || '/' || name as full_path from files;`;
    const expected = `SELECT
  path || '/' || name AS full_path
FROM files;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats NOT IN', () => {
    const input = `select * from slice where id not in (select parent_id from slice where parent_id is not null);`;
    const expected = `SELECT
  *
FROM slice
WHERE id NOT IN (SELECT parent_id FROM slice WHERE parent_id IS NOT NULL);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats NOT LIKE', () => {
    const input = `select * from slice where name not like '%internal%';`;
    const expected = `SELECT
  *
FROM slice
WHERE name NOT LIKE '%internal%';`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats NOT BETWEEN', () => {
    const input = `select * from counter where value not between 0 and 100;`;
    const expected = `SELECT
  *
FROM counter
WHERE value NOT BETWEEN 0 AND 100;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats macro invocation', () => {
    const input = `select * from _slice_following_flow!(my_table);`;
    const expected = `SELECT
  *
FROM _slice_following_flow!(my_table);`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats complex arithmetic', () => {
    const input = `select (a + b) * c / d - e % f as result from t;`;
    const expected = `SELECT
  (a + b) * c / d - e % f AS result
FROM t;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats unary minus', () => {
    const input = `select -value as neg, +value as pos from counter;`;
    const expected = `SELECT
  -value AS neg,
  +value AS pos
FROM counter;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats comparison operators', () => {
    const input = `select * from t where a = 1 and b != 2 and c <> 3 and d < 4 and e > 5 and f <= 6 and g >= 7;`;
    const expected = `SELECT
  *
FROM t
WHERE a = 1 AND b != 2 AND c <> 3 AND d < 4 AND e > 5 AND f <= 6 AND g >= 7;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats multiple CTEs with complex bodies', () => {
    const input = `with base as (select id, parent_id from slice where depth = 0), children as (select s.id from slice s join base b on s.parent_id = b.id) select * from children;`;
    const expected = `WITH
  base AS (
    SELECT
      id,
      parent_id
    FROM slice
    WHERE depth = 0
  ),
  children AS (
    SELECT
      s.id
    FROM slice AS s
    JOIN base AS b ON s.parent_id = b.id
  )
SELECT
  *
FROM children;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats CTE with column list', () => {
    const input = `with cte(x, y) as (select a, b from t) select * from cte;`;
    const expected = `WITH
  cte(x, y) AS (
    SELECT
      a,
      b
    FROM t
  )
SELECT
  *
FROM cte;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats LEFT JOIN with BETWEEN in ON', () => {
    const input = `select * from events e left join ranges r on e.ts between r.start_ts and r.end_ts;`;
    const expected = `SELECT
  *
FROM events AS e
LEFT JOIN ranges AS r ON e.ts BETWEEN r.start_ts AND r.end_ts;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats COUNT DISTINCT', () => {
    const input = `select count(distinct category) as unique_categories from slice;`;
    const expected = `SELECT
  count(DISTINCT category) AS unique_categories
FROM slice;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats nested function calls', () => {
    const input = `select coalesce(nullif(name, ''), 'unknown') as display_name from slice;`;
    const expected = `SELECT
  coalesce(nullif(name, ''), 'unknown') AS display_name
FROM slice;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats MIN/MAX aggregate functions', () => {
    const input = `select min(ts) as start_ts, max(ts + dur) as end_ts from slice group by track_id;`;
    const expected = `SELECT
  min(ts) AS start_ts,
  max(ts + dur) AS end_ts
FROM slice
GROUP BY track_id;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats SUM and AVG', () => {
    const input = `select sum(dur) as total_dur, avg(dur) as avg_dur from slice where name = 'test';`;
    const expected = `SELECT
  sum(dur) AS total_dur,
  avg(dur) AS avg_dur
FROM slice
WHERE name = 'test';`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats multiple GROUP BY columns', () => {
    const input = `select category, name, count(*) as cnt from slice group by category, name order by cnt desc;`;
    const expected = `SELECT
  category,
  name,
  count(*) AS cnt
FROM slice
GROUP BY category, name
ORDER BY cnt DESC;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats CROSS JOIN', () => {
    // Note: bare aliases conflict with CROSS/NATURAL keywords.
    // Use explicit AS to avoid ambiguity.
    const input = `select * from a as t1 cross join b;`;
    const expected = `SELECT
  *
FROM a AS t1
CROSS JOIN b;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });

  test('formats NATURAL JOIN', () => {
    const input = `select * from slice as s natural join track;`;
    const expected = `SELECT
  *
FROM slice AS s
NATURAL JOIN track;`;
    expect(norm(formatSQL(input))).toBe(norm(expected));
  });
});

describe('Format: Keyword uppercasing', () => {
  test('uppercases all SQL keywords', () => {
    const result = formatSQL(
      `select distinct x from t where x is not null and y between 1 and 10 or z like 'foo' order by x asc limit 5 offset 2;`,
    );
    expect(result).toContain('SELECT DISTINCT');
    expect(result).toContain('FROM');
    expect(result).toContain('WHERE');
    expect(result).toContain('IS NOT NULL');
    expect(result).toContain('AND');
    expect(result).toContain('BETWEEN');
    expect(result).toContain('OR');
    expect(result).toContain('LIKE');
    expect(result).toContain('ORDER BY');
    expect(result).toContain('ASC');
    expect(result).toContain('LIMIT');
    expect(result).toContain('OFFSET');
  });
});

describe('Format: Idempotency', () => {
  test('formatting already-formatted SQL produces same result', () => {
    const input = `SELECT
  id,
  name
FROM my_table
WHERE id = 1;`;
    const first = formatSQL(input);
    const second = formatSQL(first);
    expect(norm(first)).toBe(norm(second));
  });
});
