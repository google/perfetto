import {Engine} from '../../trace_processor/engine';
import {QueryResult, SqlValue} from 'src/trace_processor/query_result';

export async function runQueryForMcp(
  engine: Engine,
  query: string,
): Promise<string> {
  const result = await engine.query(query, 'PerfettoMcp');
  return resultToJson(result);
}

export async function resultToJson(result: QueryResult): Promise<string> {
  const columns = result.columns();
  const rows: unknown[] = [];
  for (const it = result.iter({}); it.valid(); it.next()) {
    const row: {[key: string]: SqlValue} = {};
    for (const name of columns) {
      let value = it.get(name);
      if (typeof value === 'bigint') {
        value = Number(value);
      }
      row[name] = value;
    }
    rows.push(row);
  }
  return JSON.stringify(rows);
}
