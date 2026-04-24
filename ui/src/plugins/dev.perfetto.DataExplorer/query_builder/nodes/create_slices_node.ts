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

import m from 'mithril';
import {
  QueryNode,
  nextNodeId,
  NodeType,
  SecondaryInputSpec,
  NodeContext,
} from '../../query_node';
import {getSecondaryInput} from '../graph_utils';
import protos from '../../../../protos';
import {ColumnInfo, columnInfoFromSqlColumn} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {loadNodeDoc} from '../node_doc_loader';
import {OutlinedField, FormListItem} from '../widgets';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {NodeTitle, ColumnName} from '../node_styling_widgets';
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';
import {
  PerfettoSqlTypes,
  typesEqual,
} from '../../../../trace_processor/perfetto_sql_type';

type TimestampMode = 'ts' | 'ts_dur';

// Output column names that CreateSlices produces
const OUTPUT_TS_COLUMN = 'ts';
const OUTPUT_DUR_COLUMN = 'dur';
const DEFAULT_TS_COLUMN = 'ts';
const COMPUTED_STARTS_END_TS_COLUMN = 'exp_tmp_starts_computed_end_ts';
const COMPUTED_ENDS_END_TS_COLUMN = 'exp_tmp_ends_computed_end_ts';

// Serializable node configuration.
export interface CreateSlicesNodeAttrs {
  startsMode?: TimestampMode;
  endsMode?: TimestampMode;
  startsTsColumn: string;
  endsTsColumn: string;
  startsDurColumn?: string;
  endsDurColumn?: string;
}

export class CreateSlicesNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kCreateSlices;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly attrs: CreateSlicesNodeAttrs;
  readonly context: NodeContext;

  get startsNode(): QueryNode | undefined {
    return getSecondaryInput(this, 0);
  }

  get endsNode(): QueryNode | undefined {
    return getSecondaryInput(this, 1);
  }

  get finalCols(): ColumnInfo[] {
    // CreateSlices always produces ts and dur columns
    if (!this.startsNode || !this.endsNode) {
      return [];
    }

    return [
      columnInfoFromSqlColumn(
        {name: OUTPUT_TS_COLUMN, type: PerfettoSqlTypes.TIMESTAMP},
        true,
      ),
      columnInfoFromSqlColumn(
        {name: OUTPUT_DUR_COLUMN, type: PerfettoSqlTypes.DURATION},
        true,
      ),
    ];
  }

  constructor(
    attrs: CreateSlicesNodeAttrs & {
      startsNode?: QueryNode;
      endsNode?: QueryNode;
    },
    context: NodeContext,
  ) {
    this.nodeId = nextNodeId();
    const {startsNode, endsNode, ...rest} = attrs;
    this.attrs = {
      ...rest,
      startsMode: rest.startsMode ?? 'ts',
      endsMode: rest.endsMode ?? 'ts',
      startsTsColumn: rest.startsTsColumn ?? DEFAULT_TS_COLUMN,
      endsTsColumn: rest.endsTsColumn ?? DEFAULT_TS_COLUMN,
      startsDurColumn: rest.startsDurColumn,
      endsDurColumn: rest.endsDurColumn,
    };
    this.context = context;
    this.secondaryInputs = {
      connections: new Map(),
      min: 2,
      max: 2,
      portNames: ['Starts', 'Ends'],
    };
    // Initialize connections from startsNode/endsNode
    if (startsNode) {
      this.secondaryInputs.connections.set(0, startsNode);
    }
    if (endsNode) {
      this.secondaryInputs.connections.set(1, endsNode);
    }
    this.nextNodes = [];
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.context.issues) {
      this.context.issues.clear();
    }

    if (
      this.secondaryInputs.connections.size !== 2 ||
      !this.startsNode ||
      !this.endsNode
    ) {
      this.setValidationError(
        'Create Slices node requires exactly two sources (starts and ends).',
      );
      return false;
    }

    if (!this.validateSourceNode(this.startsNode, 'Starts')) {
      return false;
    }

    if (!this.validateSourceNode(this.endsNode, 'Ends')) {
      return false;
    }

    // Auto-select columns if there's only one option (based on type only)
    this.autoSelectColumns();

    // Validate starts input based on mode
    if (
      !this.validateInputSource(
        this.attrs.startsMode ?? 'ts',
        this.attrs.startsTsColumn,
        this.attrs.startsDurColumn,
        this.startsNode,
        'Starts',
      )
    ) {
      return false;
    }

    // Validate ends input based on mode
    if (
      !this.validateInputSource(
        this.attrs.endsMode ?? 'ts',
        this.attrs.endsTsColumn,
        this.attrs.endsDurColumn,
        this.endsNode,
        'Ends',
      )
    ) {
      return false;
    }

    return true;
  }

  private autoSelectColumns(): void {
    if (!this.startsNode || !this.endsNode) return;

    // Auto-select timestamp columns based on type only
    const startsTimestampCols = this.startsNode.finalCols.filter(
      (c) => c.type && typesEqual(c.type, PerfettoSqlTypes.TIMESTAMP),
    );
    const endsTimestampCols = this.endsNode.finalCols.filter(
      (c) => c.type && typesEqual(c.type, PerfettoSqlTypes.TIMESTAMP),
    );

    // Auto-select starts timestamp if there's only one option
    if (startsTimestampCols.length === 1 && !this.attrs.startsTsColumn) {
      this.attrs.startsTsColumn = startsTimestampCols[0].name;
    }

    // Auto-select ends timestamp if there's only one option
    if (endsTimestampCols.length === 1 && !this.attrs.endsTsColumn) {
      this.attrs.endsTsColumn = endsTimestampCols[0].name;
    }

    // Auto-select duration columns in ts_dur mode
    if (this.attrs.startsMode === 'ts_dur') {
      const startsDurationCols = this.startsNode.finalCols.filter(
        (c) => c.type && typesEqual(c.type, PerfettoSqlTypes.DURATION),
      );
      if (startsDurationCols.length === 1 && !this.attrs.startsDurColumn) {
        this.attrs.startsDurColumn = startsDurationCols[0].name;
      }
    }

    if (this.attrs.endsMode === 'ts_dur') {
      const endsDurationCols = this.endsNode.finalCols.filter(
        (c) => c.type && typesEqual(c.type, PerfettoSqlTypes.DURATION),
      );
      if (endsDurationCols.length === 1 && !this.attrs.endsDurColumn) {
        this.attrs.endsDurColumn = endsDurationCols[0].name;
      }
    }
  }

  private validateSourceNode(node: QueryNode, nodeName: string): boolean {
    if (!node.validate()) {
      this.setValidationError(
        node.context.issues?.queryError?.message ??
          `${nodeName} node '${node.getTitle()}' is invalid`,
      );
      return false;
    }
    return true;
  }

  private validateInputSource(
    mode: TimestampMode,
    tsColumn: string,
    durColumn: string | undefined,
    sourceNode: QueryNode,
    sourceName: string,
  ): boolean {
    const cols = sourceNode.finalCols;
    const colNames = new Set(cols.map((c) => c.name));

    if (mode === 'ts') {
      if (!tsColumn) {
        this.setValidationError(`${sourceName} timestamp column is required.`);
        return false;
      }
      if (!colNames.has(tsColumn)) {
        this.setValidationError(
          `${sourceName} timestamp column '${tsColumn}' not found in ${sourceName.toLowerCase()} source.`,
        );
        return false;
      }
    } else {
      // ts_dur mode
      if (!tsColumn || !durColumn) {
        this.setValidationError(
          `Both ${sourceName.toLowerCase()} timestamp and duration columns are required for ts+dur mode.`,
        );
        return false;
      }
      if (!colNames.has(tsColumn)) {
        this.setValidationError(
          `${sourceName} timestamp column '${tsColumn}' not found in ${sourceName.toLowerCase()} source.`,
        );
        return false;
      }
      if (!colNames.has(durColumn)) {
        this.setValidationError(
          `${sourceName} duration column '${durColumn}' not found in ${sourceName.toLowerCase()} source.`,
        );
        return false;
      }
    }

    return true;
  }

  private setValidationError(message: string): void {
    if (!this.context.issues) {
      this.context.issues = new NodeIssues();
    }
    this.context.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Create Slices';
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('create_slices');
  }

  getInputLabels(): string[] {
    return ['Starts', 'Ends'];
  }

  nodeDetails(): NodeDetailsAttrs {
    const content: m.Children[] = [NodeTitle(this.getTitle())];

    if (this.attrs.startsTsColumn && this.attrs.endsTsColumn) {
      content.push(
        m(
          '.pf-exp-create-slices-details',
          m('div', [
            m('span', 'Start: '),
            ColumnName(this.attrs.startsTsColumn),
          ]),
          m('div', [m('span', 'End: '), ColumnName(this.attrs.endsTsColumn)]),
        ),
      );
    }

    return {
      content,
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();
    const error = this.context.issues?.queryError;

    const sections: NodeModifyAttrs['sections'] = [];

    // Add error if present
    if (error) {
      sections.push({
        content: m(Callout, {icon: 'error'}, error.message),
      });
    }

    // Add starts input section
    if (this.startsNode) {
      const startsMode = this.attrs.startsMode ?? 'ts';
      const startsValid = this.validateInputConfig(
        this.startsNode,
        startsMode,
        this.attrs.startsTsColumn,
        this.attrs.startsDurColumn,
      );

      sections.push({
        title: 'Starts Input',
        content: m(FormListItem, {
          item: {},
          isValid: startsValid,
          onUpdate: () => {},
          children: this.renderInputDetails(
            this.startsNode,
            startsMode,
            this.attrs.startsTsColumn,
            this.attrs.startsDurColumn,
            (mode) => {
              this.attrs.startsMode = mode;
              this.context.onchange?.();
            },
            (ts, dur) => {
              this.attrs.startsTsColumn = ts;
              if (dur !== undefined) {
                this.attrs.startsDurColumn = dur;
              }
              this.context.onchange?.();
            },
          ),
        }),
      });
    }

    // Add ends input section
    if (this.endsNode) {
      const endsMode = this.attrs.endsMode ?? 'ts';
      const endsValid = this.validateInputConfig(
        this.endsNode,
        endsMode,
        this.attrs.endsTsColumn,
        this.attrs.endsDurColumn,
      );

      sections.push({
        title: 'Ends Input',
        content: m(FormListItem, {
          item: {},
          isValid: endsValid,
          onUpdate: () => {},
          children: this.renderInputDetails(
            this.endsNode,
            endsMode,
            this.attrs.endsTsColumn,
            this.attrs.endsDurColumn,
            (mode) => {
              this.attrs.endsMode = mode;
              this.context.onchange?.();
            },
            (ts, dur) => {
              this.attrs.endsTsColumn = ts;
              if (dur !== undefined) {
                this.attrs.endsDurColumn = dur;
              }
              this.context.onchange?.();
            },
          ),
        }),
      });
    }

    return {
      info: 'Configure the start and end timestamps for creating slices. Each input can use either a single timestamp column or combine timestamp and duration columns. The columns have to have proper types, if needed change type with Modify Column node.',
      sections,
    };
  }

  private validateInputConfig(
    node: QueryNode,
    mode: TimestampMode,
    tsColumn: string,
    durColumn: string | undefined,
  ): boolean {
    const cols = node.finalCols;
    const colNames = new Set(cols.map((c) => c.name));

    // Check timestamp column
    if (!tsColumn || !colNames.has(tsColumn)) {
      return false;
    }

    // If in ts_dur mode, check duration column too
    if (mode === 'ts_dur') {
      if (!durColumn || !colNames.has(durColumn)) {
        return false;
      }
    }

    return true;
  }

  private renderInputDetails(
    node: QueryNode,
    mode: TimestampMode,
    tsColumn: string,
    durColumn: string | undefined,
    onModeChange: (mode: TimestampMode) => void,
    onColumnsChange: (ts: string, dur?: string) => void,
  ): m.Children {
    const cols = node.finalCols;
    const timestampCols = cols.filter(
      (c) => c.type && typesEqual(c.type, PerfettoSqlTypes.TIMESTAMP),
    );
    const durationCols = cols.filter(
      (c) => c.type && typesEqual(c.type, PerfettoSqlTypes.DURATION),
    );

    // Display the auto-selected column if there's only one option
    const autoTsColumn =
      timestampCols.length === 1 ? timestampCols[0].name : tsColumn;
    const autoDurColumn =
      durationCols.length === 1 ? durationCols[0].name : durColumn;

    return [
      // Mode selector
      m(
        OutlinedField,
        {
          label: 'Mode',
          value: mode,
          onchange: (e: Event) => {
            const value = (e.target as HTMLSelectElement)
              .value as TimestampMode;
            onModeChange(value);
          },
        },
        [
          m('option', {value: 'ts'}, 'Timestamp'),
          m('option', {value: 'ts_dur'}, 'Timestamp End'),
        ],
      ),

      // Timestamp column selector
      m(
        OutlinedField,
        {
          label: mode === 'ts' ? 'Timestamp Column' : 'Start Timestamp',
          value: autoTsColumn,
          disabled: timestampCols.length === 1,
          onchange: (e: Event) => {
            const value = (e.target as HTMLSelectElement).value;
            onColumnsChange(value, autoDurColumn);
          },
        },
        [
          m('option', {value: '', disabled: true}, 'Select column...'),
          ...timestampCols.map((col) =>
            m(
              'option',
              {value: col.name, selected: col.name === autoTsColumn},
              col.name,
            ),
          ),
        ],
      ),

      // Duration column selector (only in ts_dur mode)
      mode === 'ts_dur' &&
        m(
          OutlinedField,
          {
            label: 'Duration Column',
            value: autoDurColumn ?? '',
            disabled: durationCols.length === 1,
            onchange: (e: Event) => {
              const value = (e.target as HTMLSelectElement).value;
              onColumnsChange(autoTsColumn, value);
            },
          },
          [
            m('option', {value: '', disabled: true}, 'Select column...'),
            ...durationCols.map((col) =>
              m(
                'option',
                {value: col.name, selected: col.name === autoDurColumn},
                col.name,
              ),
            ),
          ],
        ),
    ];
  }

  clone(): QueryNode {
    return new CreateSlicesNode(
      {
        startsMode: this.attrs.startsMode ?? 'ts',
        endsMode: this.attrs.endsMode ?? 'ts',
        startsTsColumn: this.attrs.startsTsColumn,
        endsTsColumn: this.attrs.endsTsColumn,
        startsDurColumn: this.attrs.startsDurColumn,
        endsDurColumn: this.attrs.endsDurColumn,
      },
      this.context,
    );
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate() || !this.startsNode || !this.endsNode) return;

    const startsQuery = this.startsNode.getStructuredQuery();
    const endsQuery = this.endsNode.getStructuredQuery();

    if (!startsQuery || !endsQuery) return;

    // Process starts input
    const startsResult = this.processInputQuery(
      startsQuery,
      this.attrs.startsMode ?? 'ts',
      this.attrs.startsTsColumn,
      this.attrs.startsDurColumn,
      this.startsNode.finalCols,
      'starts',
      COMPUTED_STARTS_END_TS_COLUMN,
    );

    // Process ends input
    const endsResult = this.processInputQuery(
      endsQuery,
      this.attrs.endsMode ?? 'ts',
      this.attrs.endsTsColumn,
      this.attrs.endsDurColumn,
      this.endsNode.finalCols,
      'ends',
      COMPUTED_ENDS_END_TS_COLUMN,
    );

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;

    const createSlices =
      new protos.PerfettoSqlStructuredQuery.ExperimentalCreateSlices();
    createSlices.startsQuery = startsResult.query;
    createSlices.endsQuery = endsResult.query;
    createSlices.startsTsColumn = startsResult.tsColumnName;
    createSlices.endsTsColumn = endsResult.tsColumnName;

    sq.experimentalCreateSlices = createSlices;

    return sq;
  }

  private processInputQuery(
    query: protos.PerfettoSqlStructuredQuery,
    mode: TimestampMode,
    tsColumn: string,
    durColumn: string | undefined,
    finalCols: ColumnInfo[],
    inputName: string,
    computedColName: string,
  ): {query: protos.PerfettoSqlStructuredQuery; tsColumnName: string} {
    // If not in ts_dur mode, use the original query and column
    if (mode !== 'ts_dur' || !durColumn) {
      return {query, tsColumnName: tsColumn};
    }

    // In ts_dur mode, add computed column for end timestamp
    const allCols: ColumnSpec[] = finalCols.map((col) => ({
      columnNameOrExpression: col.name,
    }));

    // Add computed column: ts + dur
    allCols.push({
      columnNameOrExpression: `${tsColumn} + ${durColumn}`,
      alias: computedColName,
    });

    // Pass the query directly (not wrapped in a node) so extractQueryId
    // can get the ID from the proto object.
    const processedQuery =
      StructuredQueryBuilder.withSelectColumns(
        query,
        allCols,
        undefined,
        `${this.nodeId}_${inputName}_computed`,
      ) ?? query;

    return {query: processedQuery, tsColumnName: computedColName};
  }
}
