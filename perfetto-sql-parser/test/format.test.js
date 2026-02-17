import {describe, it} from 'node:test';
import assert from 'node:assert/strict';
import {formatSQL} from '../src/format.js';

// Helper: normalize whitespace for comparison - trim lines and collapse blank lines.
function norm(s) {
  return s.trim().split('\n').map(l => l.trimEnd()).join('\n');
}

// ============================================================================
// 1. INCLUDE PERFETTO MODULE
// ============================================================================
describe('Format: INCLUDE PERFETTO MODULE', () => {
  it('formats simple include', () => {
    const input = `include perfetto module android.frames.timeline;`;
    const expected = `INCLUDE PERFETTO MODULE android.frames.timeline;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats multiple includes', () => {
    const input = `
      include perfetto module intervals.intersect;
      include perfetto module slices.with_context;
    `;
    const expected = `INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE slices.with_context;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 2. Basic SELECT
// ============================================================================
describe('Format: SELECT statements', () => {
  it('formats basic SELECT with FROM and WHERE', () => {
    const input = `select id, name from my_table where id = 1;`;
    const expected = `SELECT
  id,
  name
FROM my_table
WHERE id = 1;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats SELECT *', () => {
    const input = `select * from foo;`;
    const expected = `SELECT
  *
FROM foo;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats SELECT DISTINCT', () => {
    const input = `select distinct name from items;`;
    const expected = `SELECT DISTINCT
  name
FROM items;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats SELECT with aliases', () => {
    const input = `select a.id as aid, b.name as bname from tbl as a;`;
    const expected = `SELECT
  a.id AS aid,
  b.name AS bname
FROM tbl AS a;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats SELECT with GROUP BY and ORDER BY', () => {
    const input = `select name, count() as cnt from items group by name order by cnt desc;`;
    const expected = `SELECT
  name,
  count() AS cnt
FROM items
GROUP BY name
ORDER BY cnt DESC;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats SELECT with JOIN and ON', () => {
    const input = `select a.x, b.y from a join b on a.id = b.id;`;
    const expected = `SELECT
  a.x,
  b.y
FROM a
JOIN b ON a.id = b.id;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats SELECT with LEFT JOIN and USING', () => {
    const input = `select * from a left join b using (id);`;
    const expected = `SELECT
  *
FROM a
LEFT JOIN b USING (id);`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats SELECT with LIMIT and OFFSET', () => {
    const input = `select id from t limit 10 offset 5;`;
    const expected = `SELECT
  id
FROM t
LIMIT 10 OFFSET 5;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats UNION ALL', () => {
    const input = `select * from a union all select * from b;`;
    const expected = `SELECT
  *
FROM a
UNION ALL
SELECT
  *
FROM b;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats SELECT with window function', () => {
    const input = `select id, lag(id) over (partition by utid order by ts) as prev_id from events;`;
    const expected = `SELECT
  id,
  lag(id) OVER (PARTITION BY utid ORDER BY ts) AS prev_id
FROM events;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats CASE expression', () => {
    const input = `select case when x = 1 then 'a' when x = 2 then 'b' else 'c' end as val from t;`;
    const expected = `SELECT
  CASE WHEN x = 1 THEN 'a' WHEN x = 2 THEN 'b' ELSE 'c' END AS val
FROM t;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats CAST expression', () => {
    const input = `select cast(x as INTEGER) from t;`;
    const expected = `SELECT
  CAST(x AS INTEGER)
FROM t;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats HAVING clause', () => {
    const input = `select name, count() as c from t group by name having c > 5;`;
    const expected = `SELECT
  name,
  count() AS c
FROM t
GROUP BY name
HAVING c > 5;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats IN subquery', () => {
    const input = `select * from a where id in (select id from b);`;
    const expected = `SELECT
  *
FROM a
WHERE id IN (SELECT id FROM b);`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 3. WITH / CTE
// ============================================================================
describe('Format: WITH statements', () => {
  it('formats simple CTE', () => {
    const input = `with cte as (select 1 as x) select * from cte;`;
    const expected = `WITH
  cte AS (
    SELECT
      1 AS x
  )
SELECT
  *
FROM cte;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats multiple CTEs', () => {
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
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats MATERIALIZED CTE', () => {
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
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 4. CREATE PERFETTO TABLE
// ============================================================================
describe('Format: CREATE PERFETTO TABLE', () => {
  it('formats simple CREATE PERFETTO TABLE', () => {
    const input = `create perfetto table my_table as select * from source;`;
    const expected = `CREATE PERFETTO TABLE my_table AS
SELECT
  *
FROM source;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats CREATE PERFETTO TABLE with column defs', () => {
    const input = `create perfetto table my_table(name STRING, id JOINID(track.id)) as select name, id from source;`;
    const expected = `CREATE PERFETTO TABLE my_table(
  name STRING,
  id JOINID(track.id)
) AS
SELECT
  name,
  id
FROM source;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats CREATE PERFETTO TABLE with UNION ALL', () => {
    const input = `create perfetto table combined as select * from a union all select * from b;`;
    const expected = `CREATE PERFETTO TABLE combined AS
SELECT
  *
FROM a
UNION ALL
SELECT
  *
FROM b;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 5. CREATE PERFETTO VIEW
// ============================================================================
describe('Format: CREATE PERFETTO VIEW', () => {
  it('formats simple CREATE PERFETTO VIEW', () => {
    const input = `create perfetto view my_view as select id, name from source;`;
    const expected = `CREATE PERFETTO VIEW my_view AS
SELECT
  id,
  name
FROM source;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats CREATE PERFETTO VIEW with column defs', () => {
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
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 6. CREATE PERFETTO FUNCTION
// ============================================================================
describe('Format: CREATE PERFETTO FUNCTION', () => {
  it('formats function returning TABLE', () => {
    const input = `create perfetto function _get_rate(event STRING) returns table(ts TIMESTAMP, dur DURATION) as select ts, dur from counter;`;
    const expected = `CREATE PERFETTO FUNCTION _get_rate(event STRING)
RETURNS TABLE(ts TIMESTAMP, dur DURATION)
AS
SELECT
  ts,
  dur
FROM counter;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });

  it('formats function with DELEGATES TO', () => {
    const input = `create perfetto function _tree_constraint(column STRING, op STRING, value ANY) returns ANY delegates to __intrinsic_tree_constraint;`;
    const expected = `CREATE PERFETTO FUNCTION _tree_constraint(column STRING, op STRING, value ANY)
RETURNS ANY
DELEGATES TO __intrinsic_tree_constraint;`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 7. CREATE PERFETTO MACRO
// ============================================================================
describe('Format: CREATE PERFETTO MACRO', () => {
  it('formats simple macro', () => {
    const input = `create perfetto macro my_macro(src TableOrSubquery) returns TableOrSubquery as (select * from $src);`;
    const expected = `CREATE PERFETTO MACRO my_macro(src TableOrSubquery)
RETURNS TableOrSubquery
AS (
SELECT
  *
FROM $src
);`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 8. CREATE PERFETTO INDEX
// ============================================================================
describe('Format: CREATE PERFETTO INDEX', () => {
  it('formats simple index', () => {
    const input = `create perfetto index my_idx on my_table(col1, col2);`;
    const expected = `CREATE PERFETTO INDEX my_idx
  ON my_table(col1, col2);`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 9. CREATE VIRTUAL TABLE
// ============================================================================
describe('Format: CREATE VIRTUAL TABLE', () => {
  it('formats virtual table', () => {
    const input = `create virtual table _arm_l3_rates using SPAN_OUTER_JOIN(_arm_l3_miss_rate, _arm_l3_hit_rate);`;
    const expected = `CREATE VIRTUAL TABLE _arm_l3_rates USING SPAN_OUTER_JOIN(_arm_l3_miss_rate, _arm_l3_hit_rate);`;
    assert.equal(norm(formatSQL(input)), norm(expected));
  });
});

// ============================================================================
// 10. Keyword uppercasing
// ============================================================================
describe('Format: Keyword uppercasing', () => {
  it('uppercases all SQL keywords', () => {
    const result = formatSQL(`select distinct x from t where x is not null and y between 1 and 10 or z like 'foo' order by x asc limit 5 offset 2;`);
    assert.ok(result.includes('SELECT DISTINCT'));
    assert.ok(result.includes('FROM'));
    assert.ok(result.includes('WHERE'));
    assert.ok(result.includes('IS NOT NULL'));
    assert.ok(result.includes('AND'));
    assert.ok(result.includes('BETWEEN'));
    assert.ok(result.includes('OR'));
    assert.ok(result.includes('LIKE'));
    assert.ok(result.includes('ORDER BY'));
    assert.ok(result.includes('ASC'));
    assert.ok(result.includes('LIMIT'));
    assert.ok(result.includes('OFFSET'));
  });
});

// ============================================================================
// 11. Idempotency
// ============================================================================
describe('Format: Idempotency', () => {
  it('formatting already-formatted SQL produces same result', () => {
    const input = `SELECT
  id,
  name
FROM my_table
WHERE id = 1;`;
    const first = formatSQL(input);
    const second = formatSQL(first);
    assert.equal(norm(first), norm(second));
  });
});
