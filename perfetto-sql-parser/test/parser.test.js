import {describe, it} from 'node:test';
import assert from 'node:assert/strict';
import {parser} from '../src/parser.js';

// Helper: parse SQL and return the tree as a string (S-expression style).
function parseTree(sql) {
  const tree = parser.parse(sql);
  return tree.toString();
}

// Helper: check that the parse has NO error nodes.
function assertNoErrors(sql, label) {
  const tree = parser.parse(sql);
  const str = tree.toString();
  const hasError = str.includes('⚠');
  if (hasError) {
    // Print tree for debugging
    console.log(`\n--- PARSE ERROR in: ${label || 'unnamed'} ---`);
    console.log('SQL:', sql.slice(0, 200), sql.length > 200 ? '...' : '');
    console.log('Tree:', str.slice(0, 1000));
    console.log('---');
  }
  assert.ok(!hasError, `Parse errors found in: ${label || sql.slice(0, 80)}`);
}

// Helper: check that a specific AST node type exists in the tree.
function assertHasNode(sql, nodeType, label) {
  const tree = parser.parse(sql);
  const str = tree.toString();
  assert.ok(
    str.includes(nodeType),
    `Expected node type '${nodeType}' in: ${label || sql.slice(0, 80)}\nTree: ${str.slice(0, 500)}`
  );
}

// ============================================================================
// 1. INCLUDE PERFETTO MODULE
// ============================================================================
describe('INCLUDE PERFETTO MODULE', () => {
  it('parses simple module include', () => {
    const sql = `INCLUDE PERFETTO MODULE android.frames.timeline;`;
    assertNoErrors(sql, 'simple include');
    assertHasNode(sql, 'IncludeModuleStatement', 'include statement');
    assertHasNode(sql, 'ModulePath', 'module path');
  });

  it('parses nested module path', () => {
    const sql = `INCLUDE PERFETTO MODULE wattson.cpu.idle;`;
    assertNoErrors(sql, 'nested module path');
  });

  it('parses multiple includes', () => {
    const sql = `
      INCLUDE PERFETTO MODULE intervals.intersect;
      INCLUDE PERFETTO MODULE slices.with_context;
      INCLUDE PERFETTO MODULE graphs.hierarchy;
    `;
    assertNoErrors(sql, 'multiple includes');
  });
});

// ============================================================================
// 2. SELECT statements
// ============================================================================
describe('SELECT statements', () => {
  it('parses basic SELECT with JOINs and string functions', () => {
    const sql = `
      SELECT
        str_split(str_split(slice.name, '=', 3), ')', 0) AS event_type,
        thread.tid,
        thread.name AS thread_name,
        process.upid,
        slice.ts,
        slice.dur,
        slice.track_id
      FROM slice
      JOIN thread_track ON thread_track.id = slice.track_id
      JOIN thread USING (utid)
      JOIN process USING (upid)
      WHERE slice.name GLOB 'sendMessage(*'
      ORDER BY event_seq
    `;
    assertNoErrors(sql, 'SELECT with JOINs');
    assertHasNode(sql, 'SelectStatement', 'select');
    assertHasNode(sql, 'FromClause', 'from');
    assertHasNode(sql, 'JoinClause', 'join');
    assertHasNode(sql, 'WhereClause', 'where');
    assertHasNode(sql, 'OrderByClause', 'order by');
  });

  it('parses SELECT with window functions (LAG, LEAD)', () => {
    const sql = `
      SELECT
        utid,
        id,
        ts,
        lag(id) OVER (PARTITION BY utid ORDER BY ts) AS prev_id,
        lead(id) OVER (PARTITION BY utid ORDER BY ts) AS next_id,
        coalesce(lead(idle_ts) OVER (PARTITION BY utid ORDER BY ts), thread_end_ts) - ts AS dur
      FROM _wakeup_events
      ORDER BY id
    `;
    assertNoErrors(sql, 'SELECT with window functions');
    assertHasNode(sql, 'WindowOver', 'window over');
    assertHasNode(sql, 'WindowBody', 'window body');
  });

  it('parses SELECT with GROUP BY and aggregation', () => {
    const sql = `
      SELECT
        coalesce(c.deobfuscated_name, c.name) AS class_name,
        o.heap_type,
        sum(o.self_size) AS total_size,
        count() AS count
      FROM heap_graph_object AS o
      JOIN heap_graph_class AS c ON o.type_id = c.id
      GROUP BY class_name, o.heap_type
    `;
    assertNoErrors(sql, 'SELECT with GROUP BY');
    assertHasNode(sql, 'GroupByClause', 'group by');
  });

  it('parses SELECT with extract_arg function', () => {
    const sql = `
      SELECT
        id,
        name,
        NULL AS parent_id,
        counter_unit AS unit,
        extract_arg(source_arg_set_id, 'description') AS description
      FROM __intrinsic_track
      WHERE event_type = 'counter'
    `;
    assertNoErrors(sql, 'SELECT with extract_arg');
  });

  it('parses SELECT with rate calculation using LEAD', () => {
    const sql = `
      SELECT
        ts,
        lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur,
        value / (
          lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts
        ) AS access_rate
      FROM counter AS c
      JOIN counter_track AS t
        ON c.track_id = t.id
      WHERE
        t.name = 'arm_l3_cache_miss'
    `;
    assertNoErrors(sql, 'SELECT with LEAD rate calc');
  });

  it('parses SELECT with UNION ALL', () => {
    const sql = `
      SELECT * FROM table_a
      UNION ALL
      SELECT * FROM table_b
    `;
    assertNoErrors(sql, 'UNION ALL');
    assertHasNode(sql, 'SetOperation', 'set operation');
  });

  it('parses SELECT with subquery in WHERE', () => {
    const sql = `
      SELECT id, name
      FROM slice
      WHERE track_id NOT IN (SELECT id FROM process_track)
    `;
    assertNoErrors(sql, 'NOT IN subquery');
  });

  it('parses SELECT with CASE expression', () => {
    const sql = `
      SELECT
        CASE
          WHEN state = 'R' THEN 'Running'
          WHEN state = 'S' THEN 'Sleeping'
          ELSE 'Other'
        END AS state_name
      FROM thread_state
    `;
    assertNoErrors(sql, 'CASE expression');
    assertHasNode(sql, 'CaseExpr', 'case');
  });

  it('parses SELECT with CAST', () => {
    const sql = `SELECT CAST(frame_id AS LONG) FROM events`;
    assertNoErrors(sql, 'CAST');
    assertHasNode(sql, 'CastExpr', 'cast');
  });

  it('parses SELECT with iif function', () => {
    const sql = `
      SELECT
        iif(is_irq, 'IRQ', state) AS idle_state,
        iif(is_irq, NULL, _wakeup_map.id) AS waker_id
      FROM _wakeup
    `;
    assertNoErrors(sql, 'iif function');
  });

  it('parses SELECT with LEFT JOIN and complex ON', () => {
    const sql = `
      SELECT
        s_read.id AS id_reader,
        s_disp.id AS id_dispatch
      FROM android_input_events AS e
      LEFT JOIN slice AS s_read
        ON s_read.ts = e.read_time AND s_read.track_id != 0
      LEFT JOIN slice AS s_disp
        ON s_disp.ts = e.dispatch_ts AND s_disp.track_id = e.dispatch_track_id
    `;
    assertNoErrors(sql, 'LEFT JOIN with complex ON');
  });

  it('parses SELECT with MacroVariable in WHERE', () => {
    const sql = `
      SELECT id, name
      FROM counter_track AS t
      WHERE t.name = $event
    `;
    assertNoErrors(sql, 'MacroVariable');
  });

  it('parses SELECT with Star', () => {
    const sql = `SELECT * FROM slice`;
    assertNoErrors(sql, 'SELECT *');
  });

  it('parses SELECT with DISTINCT', () => {
    const sql = `SELECT DISTINCT name FROM slice`;
    assertNoErrors(sql, 'SELECT DISTINCT');
  });
});

// ============================================================================
// 3. WITH (CTE) statements
// ============================================================================
describe('WITH (CTE) statements', () => {
  it('parses simple CTE', () => {
    const sql = `
      WITH cte AS (
        SELECT id, name FROM slice
      )
      SELECT * FROM cte
    `;
    assertNoErrors(sql, 'simple CTE');
    assertHasNode(sql, 'WithStatement', 'with');
    assertHasNode(sql, 'CommonTableExpression', 'cte');
  });

  it('parses multiple CTEs', () => {
    const sql = `
      WITH
        after_first_slash(str, machine_id) AS (
          SELECT
            substr(android_build_fingerprint, instr(android_build_fingerprint, '/') + 1) AS str,
            id AS machine_id
          FROM machine
          WHERE android_build_fingerprint IS NOT NULL
        ),
        after_second_slash(str, machine_id) AS (
          SELECT
            substr(str, instr(str, '/') + 1) AS str,
            machine_id
          FROM after_first_slash
        ),
        before_colon(str, machine_id) AS (
          SELECT
            substr(str, 0, instr(str, ':')) AS str,
            machine_id
          FROM after_second_slash
        )
      SELECT str AS name, machine_id
      FROM before_colon
    `;
    assertNoErrors(sql, 'multiple CTEs');
  });

  it('parses CTE with MATERIALIZED', () => {
    const sql = `
      WITH
        metrics AS MATERIALIZED (
          SELECT
            callsite_id,
            sum(value) AS self_value
          FROM samples
          GROUP BY callsite_id
        )
      SELECT c.id, m.self_value
      FROM callstacks AS c
      LEFT JOIN metrics AS m USING (callsite_id)
    `;
    assertNoErrors(sql, 'MATERIALIZED CTE');
  });

  it('parses RECURSIVE CTE', () => {
    const sql = `
      WITH RECURSIVE
        buckets_rec(partition_col, bucket_index, ts, dur) AS (
          SELECT partition_col, 0 AS bucket_index, trace_min_ts, bucket_duration_ns
          FROM bucket_meta
          UNION ALL
          SELECT partition_col, bucket_index + 1, ts + dur, bucket_duration_ns
          FROM buckets_rec
          WHERE bucket_index + 1 < 100
        )
      SELECT
        row_number() OVER (ORDER BY partition_col, bucket_index) AS id,
        partition_col,
        bucket_index,
        ts,
        dur
      FROM buckets_rec
    `;
    assertNoErrors(sql, 'RECURSIVE CTE');
  });
});

// ============================================================================
// 4. CREATE PERFETTO TABLE
// ============================================================================
describe('CREATE PERFETTO TABLE', () => {
  it('parses table with typed columns and CTE body', () => {
    const sql = `
      CREATE PERFETTO TABLE android_device_name(
        name STRING,
        machine_id JOINID(machine.id)
      ) AS
      WITH
        cte(str, machine_id) AS (
          SELECT substr(fp, 1) AS str, id AS machine_id
          FROM machine
          WHERE fp IS NOT NULL
        )
      SELECT str AS name, machine_id
      FROM cte
    `;
    assertNoErrors(sql, 'CREATE TABLE with CTEs');
    assertHasNode(sql, 'CreatePerfettoTableStatement', 'create table');
    assertHasNode(sql, 'ColumnDefList', 'column defs');
  });

  it('parses table without column defs', () => {
    const sql = `
      CREATE PERFETTO TABLE _input_event_frame_association AS
      SELECT * FROM _input_event_frame_intersections
      UNION ALL
      SELECT * FROM _input_event_frame_speculative_matches
    `;
    assertNoErrors(sql, 'CREATE TABLE without col defs');
  });

  it('parses table with window functions', () => {
    const sql = `
      CREATE PERFETTO TABLE _wakeup_graph AS
      SELECT
        utid,
        id,
        lag(id) OVER (PARTITION BY utid ORDER BY ts) AS prev_id,
        lead(id) OVER (PARTITION BY utid ORDER BY ts) AS next_id
      FROM _wakeup_events
      ORDER BY id
    `;
    assertNoErrors(sql, 'CREATE TABLE with window functions');
  });
});

// ============================================================================
// 5. CREATE PERFETTO VIEW
// ============================================================================
describe('CREATE PERFETTO VIEW', () => {
  it('parses view with typed columns', () => {
    const sql = `
      CREATE PERFETTO VIEW counter_track(
        id ID(track.id),
        name STRING,
        parent_id JOINID(track.id),
        type STRING,
        dimension_arg_set_id ARGSETID,
        machine_id JOINID(machine.id),
        unit STRING,
        description STRING
      ) AS
      SELECT
        id, name, NULL AS parent_id, type,
        dimension_arg_set_id,
        source_arg_set_id,
        machine_id,
        counter_unit AS unit,
        extract_arg(source_arg_set_id, 'description') AS description
      FROM __intrinsic_track
      WHERE event_type = 'counter'
    `;
    assertNoErrors(sql, 'CREATE VIEW with typed columns');
    assertHasNode(sql, 'CreatePerfettoViewStatement', 'create view');
    assertHasNode(sql, 'ColumnDefList', 'column defs');
  });

  it('parses view with UNION ALL', () => {
    const sql = `
      CREATE PERFETTO VIEW _viz_slices AS
      SELECT * FROM thread_or_process_slice
      UNION ALL
      SELECT
        slice.id,
        slice.ts,
        slice.dur,
        track.name AS track_name,
        NULL AS thread_name
      FROM slice
      JOIN track ON slice.track_id = track.id
      WHERE NOT (slice.track_id IN (SELECT id FROM process_track))
    `;
    assertNoErrors(sql, 'CREATE VIEW with UNION ALL');
  });

  it('parses view with hash and GROUP_CONCAT', () => {
    const sql = `
      CREATE PERFETTO VIEW slice_with_stack_id(
        id ID(slice.id),
        ts TIMESTAMP,
        dur DURATION,
        track_id JOINID(track.id),
        name STRING,
        stack_id LONG,
        parent_stack_id LONG
      ) AS
      WITH
        slice_stack_hashes AS (
          SELECT
            s.id,
            coalesce(
              (
                SELECT hash(GROUP_CONCAT(hash(coalesce(category, '') || '|' || name), '|'))
                FROM _slice_ancestor_and_self(s.id)
                ORDER BY depth ASC
              ),
              0
            ) AS stack_hash
          FROM slice AS s
        )
      SELECT
        s.id, s.ts, s.dur, s.track_id, s.name,
        sh.stack_hash AS stack_id,
        coalesce(parent_sh.stack_hash, 0) AS parent_stack_id
      FROM slice AS s
      JOIN slice_stack_hashes AS sh ON s.id = sh.id
      LEFT JOIN slice_stack_hashes AS parent_sh ON s.parent_id = parent_sh.id
    `;
    assertNoErrors(sql, 'CREATE VIEW with hash/GROUP_CONCAT');
  });
});

// ============================================================================
// 6. CREATE PERFETTO FUNCTION
// ============================================================================
describe('CREATE PERFETTO FUNCTION', () => {
  it('parses function returning TABLE', () => {
    const sql = `
      CREATE PERFETTO FUNCTION _get_rate(
        event STRING
      )
      RETURNS TABLE(
        ts TIMESTAMP,
        dur DURATION,
        access_rate LONG
      ) AS
      SELECT
        ts,
        lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur,
        value / (
          lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts
        ) AS access_rate
      FROM counter AS c
      JOIN counter_track AS t ON c.track_id = t.id
      WHERE t.name = $event
    `;
    assertNoErrors(sql, 'FUNCTION returning TABLE');
    assertHasNode(sql, 'CreatePerfettoFunctionStatement', 'create function');
  });

  it('parses function with JOINID params', () => {
    const sql = `
      CREATE PERFETTO FUNCTION _thread_executing_span_critical_path(
        root_utid JOINID(thread.id),
        ts TIMESTAMP,
        dur DURATION
      )
      RETURNS TABLE(
        root_utid JOINID(thread.id),
        root_id LONG,
        id LONG,
        ts TIMESTAMP,
        dur DURATION,
        utid JOINID(thread.id)
      ) AS
      SELECT root_utid, root_id, id, ts, dur, utid
      FROM _critical_path_results
    `;
    assertNoErrors(sql, 'FUNCTION with JOINID params');
  });

  it('parses function with DELEGATES TO', () => {
    const sql = `
      CREATE PERFETTO FUNCTION _tree_constraint(
        column STRING,
        op STRING,
        value ANY
      )
      RETURNS ANY
      DELEGATES TO __intrinsic_tree_constraint
    `;
    assertNoErrors(sql, 'FUNCTION with DELEGATES TO');
  });
});

// ============================================================================
// 7. CREATE PERFETTO MACRO
// ============================================================================
describe('CREATE PERFETTO MACRO', () => {
  it('parses macro with TableOrSubquery param', () => {
    const sql = `
      CREATE PERFETTO MACRO _callstacks_for_stack_profile_samples(
        spc_samples TableOrSubquery
      )
      RETURNS TableOrSubquery AS
      (
        SELECT
          f.id,
          f.parent_id,
          f.name,
          m.name AS mapping_name
        FROM _callstack_spc_forest AS f
        JOIN stack_profile_mapping AS m ON f.mapping_id = m.id
      )
    `;
    assertNoErrors(sql, 'MACRO with TableOrSubquery');
    assertHasNode(sql, 'CreatePerfettoMacroStatement', 'create macro');
  });

  it('parses macro with multiple params', () => {
    const sql = `
      CREATE PERFETTO MACRO _viz_slice_ancestor_agg(
        inits TableOrSubquery,
        nodes TableOrSubquery
      )
      RETURNS TableOrSubquery
      AS
      (
        SELECT id, parent_id, name, self_dur
        FROM $nodes
      )
    `;
    assertNoErrors(sql, 'MACRO with multiple params');
  });

  it('parses macro with MATERIALIZED CTE in body', () => {
    const sql = `
      CREATE PERFETTO MACRO _callstacks_for_callsites_weighted(
        samples TableOrSubquery
      )
      RETURNS TableOrSubquery AS
      (
        WITH
          metrics AS MATERIALIZED (
            SELECT callsite_id, sum(value) AS self_value
            FROM $samples
            GROUP BY callsite_id
          )
        SELECT
          c.id,
          c.parent_id,
          c.name,
          iif(c.is_leaf, coalesce(m.self_value, 0), 0) AS self_value
        FROM callstacks AS c
        LEFT JOIN metrics AS m USING (callsite_id)
      )
    `;
    assertNoErrors(sql, 'MACRO with MATERIALIZED CTE');
  });
});

// ============================================================================
// 8. CREATE PERFETTO INDEX
// ============================================================================
describe('CREATE PERFETTO INDEX', () => {
  it('parses simple index', () => {
    const sql = `CREATE PERFETTO INDEX _input_idx ON _input_consumers_lookup(cookie);`;
    assertNoErrors(sql, 'simple index');
    assertHasNode(sql, 'CreatePerfettoIndexStatement', 'create index');
  });

  it('parses index on callsite_id', () => {
    const sql = `CREATE PERFETTO INDEX _spc_index ON _callstack_spc_forest(callsite_id);`;
    assertNoErrors(sql, 'index on callsite_id');
  });

  it('parses index on parent_id', () => {
    const sql = `CREATE PERFETTO INDEX _spc_parent_index ON _callstack_spc_forest(parent_id);`;
    assertNoErrors(sql, 'index on parent_id');
  });
});

// ============================================================================
// 9. CREATE VIRTUAL TABLE
// ============================================================================
describe('CREATE VIRTUAL TABLE', () => {
  it('parses SPAN_OUTER_JOIN', () => {
    const sql = `
      CREATE VIRTUAL TABLE _arm_l3_rates USING SPAN_OUTER_JOIN(
        _arm_l3_miss_rate,
        _arm_l3_hit_rate
      );
    `;
    assertNoErrors(sql, 'SPAN_OUTER_JOIN');
    assertHasNode(sql, 'CreateVirtualTableStatement', 'create virtual table');
  });
});

// ============================================================================
// 10. Macro invocations (in expressions/table refs)
// ============================================================================
describe('Macro invocations', () => {
  it('parses macro invocation in FROM clause', () => {
    const sql = `
      SELECT id, ts
      FROM _tree_reachable_ancestors_or_self!(
        _callstack_spc_forest,
        (SELECT f.id FROM samples AS s JOIN forest AS f USING (callsite_id))
      ) AS g
    `;
    assertNoErrors(sql, 'macro in FROM');
  });

  it('parses nested macro invocation', () => {
    const sql = `
      SELECT ts, dur
      FROM _critical_path_by_intervals!(
        (SELECT 42 AS utid, 100 AS ts, 200 AS dur),
        _wakeup_graph
      )
    `;
    assertNoErrors(sql, 'nested macro');
  });
});

// ============================================================================
// 11. Complex real-world queries
// ============================================================================
describe('Complex real-world queries', () => {
  it('parses wattson aggregation query', () => {
    const sql = `
      WITH
        windowed_active_state AS (
          SELECT
            ii.dur,
            ii.id_1 AS period_id,
            tasks.estimated_mw,
            tasks.thread_name,
            tasks.process_name,
            tasks.tid,
            tasks.pid,
            tasks.utid
          FROM _estimates_w_tasks_attribution AS tasks
        ),
        active_summary AS (
          SELECT
            period_id,
            utid,
            min(thread_name) AS thread_name,
            min(process_name) AS process_name,
            min(tid) AS tid,
            min(pid) AS pid,
            sum(estimated_mw * dur) / 1000000000 AS active_mws
          FROM windowed_active_state
          GROUP BY period_id, utid
        )
      SELECT
        a.period_id,
        a.utid,
        a.thread_name,
        a.process_name
      FROM active_summary AS a
    `;
    assertNoErrors(sql, 'wattson aggregation');
  });

  it('parses complex input events query with multiple LEFT JOINs', () => {
    const sql = `
      SELECT
        e.input_event_id AS input_id,
        e.event_channel AS channel,
        e.end_to_end_latency_dur AS total_latency,
        s_read.id AS id_reader,
        s_read.track_id AS track_reader,
        s_disp.id AS id_dispatch,
        s_recv.id AS id_receive
      FROM android_input_events AS e
      LEFT JOIN slice AS s_read
        ON s_read.ts = e.read_time AND s_read.track_id != 0
      LEFT JOIN slice AS s_disp
        ON s_disp.ts = e.dispatch_ts AND s_disp.track_id = e.dispatch_track_id
      LEFT JOIN slice AS s_recv
        ON s_recv.ts = e.receive_ts AND s_recv.track_id = e.receive_track_id
      WHERE
        $slice_id IN (s_read.id, s_disp.id, s_recv.id)
    `;
    assertNoErrors(sql, 'complex input events query');
  });

  it('parses complete CREATE TABLE with dispatch/receive JOINs', () => {
    const sql = `
      CREATE PERFETTO TABLE android_input_events(
        dispatch_latency_dur DURATION,
        handling_latency_dur DURATION,
        tid LONG,
        thread_name STRING,
        upid JOINID(process.upid),
        pid LONG,
        process_name STRING,
        event_type STRING,
        event_seq STRING,
        dispatch_track_id JOINID(track.id),
        dispatch_ts TIMESTAMP,
        dispatch_dur DURATION,
        receive_track_id JOINID(track.id),
        receive_ts TIMESTAMP,
        receive_dur DURATION,
        frame_id LONG,
        is_speculative_frame BOOL
      ) AS
      WITH
        dispatch AS (
          SELECT *
          FROM _input_message_sent
          WHERE thread_name = 'InputDispatcher'
          ORDER BY event_seq, event_channel
        ),
        receive AS (
          SELECT *,
            replace(event_channel, '(client)', '(server)') AS dispatch_event_channel
          FROM _input_message_received
          WHERE NOT event_type IN ('0x2', 'FINISHED')
          ORDER BY event_seq, dispatch_event_channel
        )
      SELECT
        receive.ts - dispatch.ts AS dispatch_latency_dur,
        finish.ts - receive.ts AS handling_latency_dur,
        finish.tid AS tid,
        finish.thread_name AS thread_name,
        dispatch.event_type,
        dispatch.event_seq,
        dispatch.event_channel,
        dispatch.track_id AS dispatch_track_id,
        dispatch.ts AS dispatch_ts,
        dispatch.dur AS dispatch_dur,
        receive.ts AS receive_ts,
        receive.dur AS receive_dur,
        receive.track_id AS receive_track_id
      FROM dispatch
      JOIN receive
        ON receive.dispatch_event_channel = dispatch.event_channel
        AND dispatch.event_seq = receive.event_seq
    `;
    assertNoErrors(sql, 'complete CREATE TABLE');
  });

  it('parses combined program with multiple statements', () => {
    const sql = `
      INCLUDE PERFETTO MODULE intervals.intersect;
      INCLUDE PERFETTO MODULE wattson.cpu.idle;

      CREATE PERFETTO INDEX _input_idx ON _input_lookup(cookie);

      CREATE PERFETTO TABLE _results AS
      SELECT id, name FROM source_table
      WHERE active = TRUE;

      SELECT * FROM _results;
    `;
    assertNoErrors(sql, 'multi-statement program');
  });
});

// ============================================================================
// 12. Expression edge cases
// ============================================================================
describe('Expression edge cases', () => {
  it('parses IS NOT NULL', () => {
    const sql = `SELECT * FROM t WHERE x IS NOT NULL`;
    assertNoErrors(sql, 'IS NOT NULL');
  });

  it('parses IS NULL', () => {
    const sql = `SELECT * FROM t WHERE x IS NULL`;
    assertNoErrors(sql, 'IS NULL');
  });

  it('parses BETWEEN', () => {
    const sql = `SELECT * FROM t WHERE x BETWEEN 1 AND 10`;
    assertNoErrors(sql, 'BETWEEN');
  });

  it('parses NOT IN', () => {
    const sql = `SELECT * FROM t WHERE x NOT IN (1, 2, 3)`;
    assertNoErrors(sql, 'NOT IN list');
  });

  it('parses nested function calls', () => {
    const sql = `SELECT coalesce(lead(idle_ts) OVER (PARTITION BY utid ORDER BY ts), thread_end_ts) FROM t`;
    assertNoErrors(sql, 'nested function calls');
  });

  it('parses arithmetic with division and multiplication', () => {
    const sql = `SELECT sum(estimated_mw * dur) / 1000000000 AS result FROM t`;
    assertNoErrors(sql, 'arithmetic');
  });

  it('parses string concatenation with ||', () => {
    const sql = `SELECT coalesce(category, '') || '|' || name FROM slice`;
    assertNoErrors(sql, 'string concatenation');
  });

  it('parses unary minus', () => {
    const sql = `SELECT -1, -x FROM t`;
    assertNoErrors(sql, 'unary minus');
  });

  it('parses NULL as expression', () => {
    const sql = `SELECT NULL AS parent_id FROM t`;
    assertNoErrors(sql, 'NULL expression');
  });

  it('parses boolean literals', () => {
    const sql = `SELECT * FROM t WHERE active = TRUE AND deleted = FALSE`;
    assertNoErrors(sql, 'boolean literals');
  });

  it('parses subquery as expression', () => {
    const sql = `SELECT (SELECT max(ts) FROM slice) AS max_ts FROM t`;
    assertNoErrors(sql, 'subquery expression');
  });

  it('parses row_number() OVER', () => {
    const sql = `SELECT row_number() OVER (ORDER BY ts) AS rn FROM t`;
    assertNoErrors(sql, 'row_number()');
  });
});
