// Copyright (C) 2024 The Android Open Source Project
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

import m from 'mithril';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {Trace} from '../../public/trace';
import {QueryResult, Row} from '../../trace_processor/query_result';
import {SqlRef} from '../../widgets/sql_ref';
import {SqlTableDefinition} from '../../components/widgets/sql/table/table_description';
import {MenuItem} from '../../widgets/menu';
import {extensions} from '../../components/extensions';

export const SCROLLS_TRACK_URI = 'perfetto.ChromeScrollJank#toplevelScrolls';
export const EVENT_LATENCY_TRACK_URI = 'perfetto.ChromeScrollJank#eventLatency';
export const JANKS_TRACK_URI = 'perfetto.ChromeScrollJank#scrollJankV3';

export function renderSliceRef(args: {
  trace: Trace;
  id: number;
  trackUri: string;
  title: m.Children;
}) {
  return m(
    Anchor,
    {
      icon: Icons.UpdateSelection,
      onclick: () => {
        args.trace.selection.selectTrackEvent(args.trackUri, args.id, {
          scrollToSelection: true,
        });
      },
    },
    args.title,
  );
}

export function renderSqlRef(args: {
  trace: Trace;
  tableName: string;
  tableDefinition: SqlTableDefinition | undefined;
  id: number | bigint;
}) {
  const tableDefinition = args.tableDefinition;
  return m(SqlRef, {
    table: args.tableName,
    id: args.id,
    additionalMenuItems: tableDefinition && [
      m(MenuItem, {
        label: 'Show query results',
        icon: 'table',
        onclick: () =>
          extensions.addLegacySqlTableTab(args.trace, {
            table: tableDefinition,
            filters: [
              {
                op: ([columnName]) => `${columnName} = ${args.id}`,
                columns: ['id'],
              },
            ],
          }),
      }),
    ],
  });
}

/**
 * Returns an array of the rows in `queryResult`.
 *
 * Warning: Only use this function in contexts where the number of rows is
 * guaranteed to be small. Prefer doing transformations in SQL where possible.
 */
export function rows<R extends Row>(queryResult: QueryResult, spec: R): R[] {
  const results: R[] = [];
  for (const it = queryResult.iter(spec); it.valid(); it.next()) {
    const row: Row = {};
    for (const key of Object.keys(spec)) {
      row[key] = it[key];
    }
    results.push(row as R);
  }
  return results;
}

/**
 * Converts a number to a boolean according to SQLite's conversion rules.
 *
 * See https://www.sqlite.org/lang_expr.html#boolean_expressions.
 */
export function fromSqlBool(value: number): boolean;
export function fromSqlBool(value: null): undefined;
export function fromSqlBool(value: number | null): boolean | undefined;
export function fromSqlBool(value: number | null): boolean | undefined {
  if (value === null) {
    return undefined;
  } else {
    return value !== 0;
  }
}
