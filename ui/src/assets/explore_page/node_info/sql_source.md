# SQL Source

**Purpose:** Write custom SQL queries to access any data in the trace. Most flexible option for complex logic or operations not available through other nodes.

**How to use:**
- Write your SQL query in the editor
- Use `$node_id` syntax to reference other nodes (e.g., `SELECT * FROM $node_123`)
- Click Run to execute the query
- The query results become available as columns for downstream nodes
- Access query history below the editor

**Data transformation:**
- No predefined transformation - you write the logic
- Can use any SQL: SELECT, JOIN, WHERE, GROUP BY, window functions, CTEs, etc.
- Can reference other nodes as tables using `$node_id`
- Can use any Perfetto SQL functions and tables
- The result columns become the output of this node

**Example 1 - Basic query:**
```sql
SELECT * FROM slice WHERE dur > 1000000
```

**Example 2 - Reference another node:**
```sql
SELECT
  name,
  COUNT(*) as count,
  AVG(dur) as avg_duration
FROM $other_node
GROUP BY name
ORDER BY count DESC
LIMIT 10
```

**Example 3 - Join multiple sources:**
```sql
SELECT
  s.name,
  p.name as process_name,
  s.dur
FROM slice s
JOIN process p ON s.upid = p.upid
WHERE s.dur > 1000000
```

**Tips:**
- Use the query history to recall previous queries
- Press Ctrl+Enter to run the query
- The `$node_id` syntax makes it easy to reference other nodes in the graph
- You have full access to Perfetto SQL's powerful extensions

**Note:** Unlike visual nodes, SQL source requires manual execution (click Run button). This gives you control over when expensive queries execute.
