WITH
  symbol AS (
    SELECT
      id,
      symbol_set_id,
      replace(replace(name, '(anonymous namespace)::', ''), '()', '') AS name
    FROM stack_profile_symbol
  ),
  symbol_agg AS (
    SELECT
      id,
      symbol_set_id,
      string_agg(name, ',')
        OVER (
          PARTITION BY symbol_set_id
          ORDER BY id DESC
          RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
        ) AS name
    FROM symbol
    WHERE name IN ('main', 'A', 'B', 'C', 'D', 'E')
  ),
  inline AS (
    SELECT symbol_set_id, name FROM symbol_agg WHERE id = symbol_set_id
  ),
  frame AS (
    SELECT f.id AS frame_id, i.name
    FROM STACK_PROFILE_FRAME f, inline i
    USING (symbol_set_id)
  ),
  child AS (
    SELECT
      spc.id,
      spc.parent_id,
      name
    FROM perf_sample s, stack_profile_callsite spc
    ON (s.callsite_id = spc.id),
    frame USING (frame_id)
    UNION ALL
    SELECT
      parent.id,
      parent.parent_id,
      COALESCE(f.name || ',', '') || child.name AS name
    FROM child, stack_profile_callsite parent
    ON (child.parent_id = parent.id)
    LEFT JOIN frame f
      USING (frame_id)
  )
SELECT DISTINCT name FROM child WHERE parent_id IS NULL ORDER BY name