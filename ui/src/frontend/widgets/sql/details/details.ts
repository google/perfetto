// Copyright (C) 2023 The Android Open Source Project
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
import {Brand} from '../../../../base/brand';
import {Time} from '../../../../base/time';
import {exists} from '../../../../base/utils';
import {raf} from '../../../../core/raf_scheduler';
import {Engine} from '../../../../trace_processor/engine';
import {Row} from '../../../../trace_processor/query_result';
import {
  SqlValue,
  sqlValueToReadableString,
} from '../../../../trace_processor/sql_utils';
import {Arg, getArgs} from '../../../../trace_processor/sql_utils/args';
import {asArgSetId} from '../../../../trace_processor/sql_utils/core_types';
import {Anchor} from '../../../../widgets/anchor';
import {renderError} from '../../../../widgets/error';
import {SqlRef} from '../../../../widgets/sql_ref';
import {Tree, TreeNode} from '../../../../widgets/tree';
import {hasArgs, renderArguments} from '../../../slice_args';
import {DurationWidget} from '../../../widgets/duration';
import {Timestamp as TimestampWidget} from '../../../widgets/timestamp';
import {sqlIdRegistry} from './sql_ref_renderer_registry';
import {Trace} from '../../../../public/trace';

// This file contains the helper to render the details tree (based on Tree
// widget) for an object represented by a SQL row in some table. The user passes
// a typed schema of the tree and this impl handles fetching and rendering.
//
// The following types are supported:
// Containers:
//  - dictionary (keys should be strings)
//  - array
// Primitive values:
//  - number, string, timestamp, duration, interval and thread interval.
//  - id into another sql table.
//  - arg set id.
//
// For each primitive value, the user should specify a SQL expression (usually
// just the column name). Each primitive value can be auto-skipped if the
// underlying SQL value is null (skipIfNull). Each container can be auto-skipped
// if empty (skipIfEmpty).
//
// Example of a schema:
// {
//  'Navigation ID': 'navigation_id',
//  'beforeunload': SqlIdRef({
//    source: 'beforeunload_slice_id',
//    table: 'chrome_frame_tree_nodes.id',
//   }),
//   'initiator_origin': String({
//      source: 'initiator_origin',
//      skipIfNull: true,
//   }),
//   'committed_render_frame_host': {
//     'Process ID' : 'committed_render_frame_host_process_id',
//     'RFH ID': 'committed_render_frame_host_rfh_id',
//   },
//   'initial_render_frame_host': Dict({
//     data: {
//       'Process ID': 'committed_render_frame_host_process_id',
//       'RFH ID': 'committed_render_frame_host_rfh_id',
//     },
//     preview: 'printf("id=%d:%d")', committed_render_frame_host_process_id,
//     committed_render_frame_host_rfh_id)', skipIfEmpty: true,
//   })
// }

// === Public API surface ===

export namespace DetailsSchema {
  // Create a dictionary object for the schema.
  export function Dict(
    args: {data: {[key: string]: ValueDesc}} & ContainerParams,
  ): DictSchema {
    return new DictSchema(args.data, {
      skipIfEmpty: args.skipIfEmpty,
    });
  }

  // Create an array object for the schema.
  export function Arr(
    args: {data: ValueDesc[]} & ContainerParams,
  ): ArraySchema {
    return new ArraySchema(args.data, {
      skipIfEmpty: args.skipIfEmpty,
    });
  }

  // Create an object representing a timestamp for the schema.
  // |ts| — SQL expression (e.g. column name) for the timestamp.
  export function Timestamp(
    ts: string,
    args?: ScalarValueParams,
  ): ScalarValueSchema {
    return new ScalarValueSchema('timestamp', ts, args);
  }

  // Create an object representing a duration for the schema.
  // |dur| — SQL expression (e.g. column name) for the duration.
  export function Duration(
    dur: string,
    args?: ScalarValueParams,
  ): ScalarValueSchema {
    return new ScalarValueSchema('duration', dur, args);
  }

  // Create an object representing a time interval (timestamp + duration)
  // for the schema.
  // |ts|, |dur| - SQL expressions (e.g. column names) for the timestamp
  // and duration.
  export function Interval(
    ts: string,
    dur: string,
    args?: ScalarValueParams,
  ): IntervalSchema {
    return new IntervalSchema(ts, dur, args);
  }

  // Create an object representing a combination of time interval and thread for
  // the schema.
  // |ts|, |dur|, |utid| - SQL expressions (e.g. column names) for the
  // timestamp, duration and unique thread id.
  export function ThreadInterval(
    ts: string,
    dur: string,
    utid: string,
    args?: ScalarValueParams,
  ): ThreadIntervalSchema {
    return new ThreadIntervalSchema(ts, dur, utid, args);
  }

  // Create an object representing a reference to an arg set for the schema.
  // |argSetId| - SQL expression (e.g. column name) for the arg set id.
  export function ArgSetId(
    argSetId: string,
    args?: ScalarValueParams,
  ): ScalarValueSchema {
    return new ScalarValueSchema('arg_set_id', argSetId, args);
  }

  // Create an object representing a SQL value for the schema.
  // |value| - SQL expression (e.g. column name) for the value.
  export function Value(
    value: string,
    args?: ScalarValueParams,
  ): ScalarValueSchema {
    return new ScalarValueSchema('value', value, args);
  }

  // Create an object representing string-rendered-as-url for the schema.
  // |value| - SQL expression (e.g. column name) for the value.
  export function URLValue(
    value: string,
    args?: ScalarValueParams,
  ): ScalarValueSchema {
    return new ScalarValueSchema('url', value, args);
  }

  export function Boolean(
    value: string,
    args?: ScalarValueParams,
  ): ScalarValueSchema {
    return new ScalarValueSchema('boolean', value, args);
  }

  // Create an object representing a reference to a SQL table row in the schema.
  // |table| - name of the table.
  // |id| - SQL expression (e.g. column name) for the id.
  export function SqlIdRef(
    table: string,
    id: string,
    args?: ScalarValueParams,
  ): SqlIdRefSchema {
    return new SqlIdRefSchema(table, id, args);
  }
} // namespace DetailsSchema

// Params which apply to scalar values (i.e. all non-dicts and non-arrays).
type ScalarValueParams = {
  skipIfNull?: boolean;
};

// Params which apply to containers (dicts and arrays).
type ContainerParams = {
  skipIfEmpty?: boolean;
};

// Definition of a node in the schema.
export type ValueDesc =
  | DictSchema
  | ArraySchema
  | ScalarValueSchema
  | IntervalSchema
  | ThreadIntervalSchema
  | SqlIdRefSchema
  | string
  | ValueDesc[]
  | {[key: string]: ValueDesc};

// Class responsible for fetching the data and rendering the data.
export class Details {
  constructor(
    private trace: Trace,
    private sqlTable: string,
    private id: number,
    schema: {[key: string]: ValueDesc},
  ) {
    this.dataController = new DataController(
      trace,
      sqlTable,
      id,
      sqlIdRegistry,
    );

    this.resolvedSchema = {
      kind: 'dict',
      data: Object.fromEntries(
        Object.entries(schema).map(([key, value]) => [
          key,
          resolve(value, this.dataController),
        ]),
      ),
    };
    this.dataController.fetch();
  }

  isLoading() {
    return this.dataController.data === undefined;
  }

  render(): m.Children {
    if (this.dataController.data === undefined) {
      return m('h2', 'Loading');
    }
    const nodes = [];
    for (const [key, value] of Object.entries(this.resolvedSchema.data)) {
      nodes.push(
        renderValue(
          this.trace,
          key,
          value,
          this.dataController.data,
          this.dataController.sqlIdRefRenderers,
        ),
      );
    }
    nodes.push(
      m(TreeNode, {
        left: 'SQL ID',
        right: m(SqlRef, {
          table: this.sqlTable,
          id: this.id,
        }),
      }),
    );
    return m(Tree, nodes);
  }

  private dataController: DataController;
  private resolvedSchema: ResolvedDict;
}

// Type corresponding to a value which can be rendered as a part of the tree:
// basically, it's TreeNode component without its left part.
export type RenderedValue = {
  // The value that should be rendered as the right part of the corresponding
  // TreeNode.
  value: m.Children;
  // Values that should be rendered as the children of the corresponding
  // TreeNode.
  children?: m.Children;
};

// Type describing how render an id into a given table, split into
// async `fetch` step for fetching data and sync `render` step for generating
// the vdom.
export type SqlIdRefRenderer = {
  fetch: (engine: Engine, id: bigint) => Promise<{} | undefined>;
  render: (data: {}) => RenderedValue;
};

// === Impl details ===

// Resolved index into the list of columns / expression to fetch.
type ExpressionIndex = Brand<number, 'expression_index'>;
// Arg sets and SQL references require a separate query to fetch the data and
// therefore are tracked separately.
type ArgSetIndex = Brand<number, 'arg_set_id_index'>;
type SqlIdRefIndex = Brand<number, 'sql_id_ref'>;

// Description is passed by the user and then the data is resolved into
// "resolved" versions of the types. Description focuses on the end-user
// ergonomics, while "Resolved" optimises for internal processing.

// Description of a dict in the schema.
class DictSchema {
  constructor(
    public data: {[key: string]: ValueDesc},
    public params?: ContainerParams,
  ) {}
}

// Resolved version of a dict.
type ResolvedDict = {
  kind: 'dict';
  data: {[key: string]: ResolvedValue};
} & ContainerParams;

// Description of an array in the schema.
class ArraySchema {
  constructor(
    public data: ValueDesc[],
    public params?: ContainerParams,
  ) {}
}

// Resolved version of an array.
type ResolvedArray = {
  kind: 'array';
  data: ResolvedValue[];
} & ContainerParams;

// Schema for all simple scalar values (ones that need to fetch only one value
// from SQL).
class ScalarValueSchema {
  constructor(
    public kind:
      | 'timestamp'
      | 'duration'
      | 'arg_set_id'
      | 'value'
      | 'url'
      | 'boolean',
    public sourceExpression: string,
    public params?: ScalarValueParams,
  ) {}
}

// Resolved version of simple scalar values.
type ResolvedScalarValue = {
  kind: 'timestamp' | 'duration' | 'value' | 'url' | 'boolean';
  source: ExpressionIndex;
} & ScalarValueParams;

// Resolved version of arg set.
type ResolvedArgSet = {
  kind: 'arg_set_id';
  source: ArgSetIndex;
} & ScalarValueParams;

// Schema for a time interval (ts, dur pair).
class IntervalSchema {
  constructor(
    public ts: string,
    public dur: string,
    public params?: ScalarValueParams,
  ) {}
}

// Resolved version of a time interval.
type ResolvedInterval = {
  kind: 'interval';
  ts: ExpressionIndex;
  dur: ExpressionIndex;
} & ScalarValueParams;

// Schema for a time interval for a given thread (ts, dur, utid triple).
class ThreadIntervalSchema {
  constructor(
    public ts: string,
    public dur: string,
    public utid: string,
    public params?: ScalarValueParams,
  ) {}
}

// Resolved version of a time interval for a given thread.
type ResolvedThreadInterval = {
  kind: 'thread_interval';
  ts: ExpressionIndex;
  dur: ExpressionIndex;
  utid: ExpressionIndex;
} & ScalarValueParams;

// Schema for a reference to a SQL table row.
class SqlIdRefSchema {
  constructor(
    public table: string,
    public id: string,
    public params?: ScalarValueParams,
  ) {}
}

type ResolvedSqlIdRef = {
  kind: 'sql_id_ref';
  ref: SqlIdRefIndex;
} & ScalarValueParams;

type ResolvedValue =
  | ResolvedDict
  | ResolvedArray
  | ResolvedScalarValue
  | ResolvedArgSet
  | ResolvedInterval
  | ResolvedThreadInterval
  | ResolvedSqlIdRef;

// Helper class to store the error messages while fetching the data.
class Err {
  constructor(public message: string) {}
}

// Fetched data from SQL which is needed to render object according to the given
// schema.
interface Data {
  // Source of the expressions that were fetched.
  valueExpressions: string[];
  // Fetched values.
  values: SqlValue[];

  // Source statements for the arg sets.
  argSetExpressions: string[];
  // Fetched arg sets.
  argSets: (Arg[] | Err)[];

  // Source statements for the SQL references.
  sqlIdRefs: {tableName: string; idExpression: string}[];
  // Fetched data for the SQL references.
  sqlIdRefData: (
    | {
        data: {};
        id: bigint | null;
      }
    | Err
  )[];
}

// Class responsible for collecting the description of the data to fetch and
// fetching it.
class DataController {
  // List of expressions to fetch. Resolved values will have indexes into this
  // list.
  expressions: string[] = [];
  // List of arg sets to fetch. Arg set ids are fetched first (together with
  // other scalar values as a part of the `expressions` list) and then the arg
  // sets themselves are fetched.
  argSets: ExpressionIndex[] = [];
  // List of SQL references to fetch. SQL reference ids are fetched first
  // (together with other scalar values as a part of the `expressions` list) and
  // then the SQL references themselves are fetched.
  sqlIdRefs: {id: ExpressionIndex; tableName: string}[] = [];

  // Fetched data.
  data?: Data;

  constructor(
    private trace: Trace,
    private sqlTable: string,
    private id: number,
    public sqlIdRefRenderers: {[table: string]: SqlIdRefRenderer},
  ) {}

  // Fetch the data. `expressions` and other lists must be populated first by
  // resolving the schema.
  async fetch() {
    const data: Data = {
      valueExpressions: this.expressions,
      values: [],
      argSetExpressions: this.argSets.map((index) => this.expressions[index]),
      argSets: [],
      sqlIdRefs: this.sqlIdRefs.map((ref) => ({
        tableName: ref.tableName,
        idExpression: this.expressions[ref.id],
      })),
      sqlIdRefData: [],
    };

    // Helper to generate the labels for the expressions.
    const label = (index: number) => `col_${index}`;

    // Fetch the scalar values for the basic expressions.
    const row: Row = (
      await this.trace.engine.query(`
      SELECT
        ${this.expressions
          .map((value, index) => `${value} as ${label(index)}`)
          .join(',\n')}
      FROM ${this.sqlTable}
      WHERE id = ${this.id}
    `)
    ).firstRow({});
    for (let i = 0; i < this.expressions.length; ++i) {
      data.values.push(row[label(i)]);
    }

    // Fetch the arg sets based on the fetched arg set ids.
    for (const argSetIndex of this.argSets) {
      const argSetId = data.values[argSetIndex];
      if (argSetId === null) {
        data.argSets.push([]);
      } else if (typeof argSetId !== 'number' && typeof argSetId !== 'bigint') {
        data.argSets.push(
          new Err(
            `Incorrect type for arg set ${
              data.argSetExpressions[argSetIndex]
            }: expected a number, got ${typeof argSetId} instead}`,
          ),
        );
      } else {
        data.argSets.push(
          await getArgs(this.trace.engine, asArgSetId(Number(argSetId))),
        );
      }
    }

    // Fetch the data for SQL references based on fetched ids.
    for (const ref of this.sqlIdRefs) {
      const renderer = this.sqlIdRefRenderers[ref.tableName];
      if (renderer === undefined) {
        data.sqlIdRefData.push(new Err(`Unknown table ${ref.tableName}`));
        continue;
      }
      const id = data.values[ref.id];
      if (id === null) {
        data.sqlIdRefData.push({data: {}, id});
        continue;
      } else if (typeof id !== 'bigint') {
        data.sqlIdRefData.push(
          new Err(
            `Incorrect type for SQL reference ${
              data.valueExpressions[ref.id]
            }: expected a bigint, got ${typeof id} instead}`,
          ),
        );
        continue;
      }
      const refData = await renderer.fetch(this.trace.engine, id);
      if (refData === undefined) {
        data.sqlIdRefData.push(
          new Err(
            `Failed to fetch the data with id ${id} for table ${ref.tableName}`,
          ),
        );
        continue;
      }
      data.sqlIdRefData.push({data: refData, id});
    }

    this.data = data;
    raf.scheduleFullRedraw();
  }

  // Add a given expression to the list of expressions to fetch and return its
  // index.
  addExpression(expr: string): ExpressionIndex {
    const result = this.expressions.length;
    this.expressions.push(expr);
    return result as ExpressionIndex;
  }

  // Add a given arg set to the list of arg sets to fetch and return its index.
  addArgSet(expr: string): ArgSetIndex {
    const result = this.argSets.length;
    this.argSets.push(this.addExpression(expr));
    return result as ArgSetIndex;
  }

  // Add a given SQL reference to the list of SQL references to fetch and return
  // its index.
  addSqlIdRef(tableName: string, idExpr: string): SqlIdRefIndex {
    const result = this.sqlIdRefs.length;
    this.sqlIdRefs.push({
      tableName,
      id: this.addExpression(idExpr),
    });
    return result as SqlIdRefIndex;
  }
}

// Resolve a given schema into a resolved version, normalising the schema and
// computing the list of data to fetch.
function resolve(schema: ValueDesc, data: DataController): ResolvedValue {
  if (typeof schema === 'string') {
    return {
      kind: 'value',
      source: data.addExpression(schema),
    };
  }
  if (Array.isArray(schema)) {
    return {
      kind: 'array',
      data: schema.map((x) => resolve(x, data)),
    };
  }
  if (schema instanceof ArraySchema) {
    return {
      kind: 'array',
      data: schema.data.map((x) => resolve(x, data)),
      ...schema.params,
    };
  }
  if (schema instanceof ScalarValueSchema) {
    if (schema.kind === 'arg_set_id') {
      return {
        kind: schema.kind,
        source: data.addArgSet(schema.sourceExpression),
        ...schema.params,
      };
    } else {
      return {
        kind: schema.kind,
        source: data.addExpression(schema.sourceExpression),
        ...schema.params,
      };
    }
  }
  if (schema instanceof IntervalSchema) {
    return {
      kind: 'interval',
      ts: data.addExpression(schema.ts),
      dur: data.addExpression(schema.dur),
      ...schema.params,
    };
  }
  if (schema instanceof ThreadIntervalSchema) {
    return {
      kind: 'thread_interval',
      ts: data.addExpression(schema.ts),
      dur: data.addExpression(schema.dur),
      utid: data.addExpression(schema.utid),
      ...schema.params,
    };
  }
  if (schema instanceof SqlIdRefSchema) {
    return {
      kind: 'sql_id_ref',
      ref: data.addSqlIdRef(schema.table, schema.id),
      ...schema.params,
    };
  }
  if (schema instanceof DictSchema) {
    return {
      kind: 'dict',
      data: Object.fromEntries(
        Object.entries(schema.data).map(([key, value]) => [
          key,
          resolve(value, data),
        ]),
      ),
      ...schema.params,
    };
  }
  return {
    kind: 'dict',
    data: Object.fromEntries(
      Object.entries(schema).map(([key, value]) => [key, resolve(value, data)]),
    ),
  };
}

// Generate the vdom for a given value using the fetched `data`.
function renderValue(
  trace: Trace,
  key: string,
  value: ResolvedValue,
  data: Data,
  sqlIdRefRenderers: {[table: string]: SqlIdRefRenderer},
): m.Children {
  switch (value.kind) {
    case 'value':
      if (data.values[value.source] === null && value.skipIfNull) return null;
      return m(TreeNode, {
        left: key,
        right: sqlValueToReadableString(data.values[value.source]),
      });
    case 'url': {
      const url = data.values[value.source];
      let rhs: m.Children;
      if (url === null) {
        if (value.skipIfNull) return null;
        rhs = renderNull();
      } else if (typeof url !== 'string') {
        rhs = renderError(
          `Incorrect type for URL ${
            data.valueExpressions[value.source]
          }: expected string, got ${typeof url}`,
        );
      } else {
        rhs = m(
          Anchor,
          {href: url, target: '_blank', icon: 'open_in_new'},
          url,
        );
      }
      return m(TreeNode, {
        left: key,
        right: rhs,
      });
    }
    case 'boolean': {
      const bool = data.values[value.source];
      if (bool === null && value.skipIfNull) return null;
      let rhs: m.Child;
      if (typeof bool !== 'bigint' && typeof bool !== 'number') {
        rhs = renderError(
          `Incorrect type for boolean ${
            data.valueExpressions[value.source]
          }: expected bigint or number, got ${typeof bool}`,
        );
      } else {
        rhs = bool ? 'true' : 'false';
      }
      return m(TreeNode, {left: key, right: rhs});
    }
    case 'timestamp': {
      const ts = data.values[value.source];
      let rhs: m.Child;
      if (ts === null) {
        if (value.skipIfNull) return null;
        rhs = m('i', 'NULL');
      } else if (typeof ts !== 'bigint') {
        rhs = renderError(
          `Incorrect type for timestamp ${
            data.valueExpressions[value.source]
          }: expected bigint, got ${typeof ts}`,
        );
      } else {
        rhs = m(TimestampWidget, {
          ts: Time.fromRaw(ts),
        });
      }
      return m(TreeNode, {
        left: key,
        right: rhs,
      });
    }
    case 'duration': {
      const dur = data.values[value.source];
      return m(TreeNode, {
        left: key,
        right:
          typeof dur === 'bigint' &&
          m(DurationWidget, {
            dur,
          }),
      });
    }
    case 'interval':
    case 'thread_interval': {
      const dur = data.values[value.dur];
      return m(TreeNode, {
        left: key,
        right:
          typeof dur === 'bigint' &&
          m(DurationWidget, {
            dur,
          }),
      });
    }
    case 'sql_id_ref':
      const ref = data.sqlIdRefs[value.ref];
      const refData = data.sqlIdRefData[value.ref];
      let rhs: m.Children;
      let children: m.Children;
      if (refData instanceof Err) {
        rhs = renderError(refData.message);
      } else if (refData.id === null && value.skipIfNull === true) {
        rhs = renderNull();
      } else {
        const renderer = sqlIdRefRenderers[ref.tableName];
        if (renderer === undefined) {
          rhs = renderError(
            `Unknown table ${ref.tableName} (${ref.tableName}[${refData.id}])`,
          );
        } else {
          const rendered = renderer.render(refData.data);
          rhs = rendered.value;
          children = rendered.children;
        }
      }
      return m(
        TreeNode,
        {
          left: key,
          right: rhs,
        },
        children,
      );
    case 'arg_set_id':
      const args = data.argSets[value.source];
      if (args instanceof Err) {
        return renderError(args.message);
      }
      return (
        hasArgs(args) &&
        m(
          TreeNode,
          {
            left: key,
          },
          renderArguments(trace, args),
        )
      );
    case 'array': {
      const children: m.Children[] = [];
      for (const child of value.data) {
        const renderedChild = renderValue(
          trace,
          `[${children.length}]`,
          child,
          data,
          sqlIdRefRenderers,
        );
        if (exists(renderedChild)) {
          children.push(renderedChild);
        }
      }
      if (children.length === 0 && value.skipIfEmpty) {
        return null;
      }
      return m(
        TreeNode,
        {
          left: key,
        },
        children,
      );
    }
    case 'dict': {
      const children: m.Children[] = [];
      for (const [key, val] of Object.entries(value.data)) {
        const child = renderValue(trace, key, val, data, sqlIdRefRenderers);
        if (exists(child)) {
          children.push(child);
        }
      }
      if (children.length === 0 && value.skipIfEmpty) {
        return null;
      }
      return m(
        TreeNode,
        {
          left: key,
        },
        children,
      );
    }
  }
}

function renderNull(): m.Children {
  return m('i', 'NULL');
}
