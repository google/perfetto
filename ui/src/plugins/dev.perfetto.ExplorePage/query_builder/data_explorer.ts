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

import {AsyncLimiter} from '../../../base/async_limiter';
import {Icons} from '../../../base/semantic_icons';
import {
  QueryResponse,
  runQueryForQueryTable,
} from '../../../components/query_table/queries';
import {
  DataGridDataSource,
  FilterDefinition,
} from '../../../components/widgets/data_grid/common';
import {
  DataGrid,
  renderCell,
} from '../../../components/widgets/data_grid/data_grid';
import {DataGridModel} from '../../../components/widgets/data_grid/common';
import {InMemoryDataSource} from '../../../components/widgets/data_grid/in_memory_data_source';
import {Trace} from '../../../public/trace';
import {SqlValue} from '../../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Callout} from '../../../widgets/callout';
import {DetailsShell} from '../../../widgets/details_shell';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {TextParagraph} from '../../../widgets/text_paragraph';
import {Query, queryToRun, NodeType, QueryNode} from '../query_node';
import {Intent} from '../../../widgets/common';
import {AggregationsOperator} from './operations/aggregations';

export interface DataExplorerAttrs {
  readonly node: QueryNode;
  readonly query?: Query | Error;
  readonly executeQuery: boolean;
  readonly trace: Trace;
  readonly onQueryExecuted: (result: {
    columns: string[];
    error?: Error;
    warning?: Error;
    noDataWarning?: Error;
  }) => void;
  readonly onPositionChange: (pos: 'left' | 'right' | 'bottom') => void;
  readonly isFullScreen: boolean;
  readonly onFullScreenToggle: () => void;
  readonly onchange?: () => void;
}

export class DataExplorer implements m.ClassComponent<DataExplorerAttrs> {
  private readonly asyncLimiter = new AsyncLimiter();
  private response?: QueryResponse;
  private dataSource?: DataGridDataSource;
  private showAggregationCard: boolean = false;

  oncreate({attrs}: m.CVnode<DataExplorerAttrs>) {
    this.runQuery(attrs);
  }

  onupdate({attrs}: m.CVnode<DataExplorerAttrs>) {
    this.runQuery(attrs);
  }

  private runQuery(attrs: DataExplorerAttrs) {
    this.asyncLimiter.schedule(async () => {
      if (
        attrs.query === undefined ||
        attrs.query instanceof Error ||
        !attrs.executeQuery
      ) {
        return;
      }

      this.response = await runQueryForQueryTable(
        queryToRun(attrs.query),
        attrs.trace.engine,
      );

      const ds = new InMemoryDataSource(this.response.rows);
      this.dataSource = {
        get rows() {
          return ds.rows;
        },
        notifyUpdate(model: DataGridModel) {
          // We override the notifyUpdate method to ignore filters, as the data is
          // assumed to be pre-filtered. We still apply sorting and aggregations.
          const newModel: DataGridModel = {
            ...model,
            filters: [], // Always pass an empty array of filters.
          };
          ds.notifyUpdate(newModel);
        },
      };

      const error = findErrors(attrs.query, this.response);
      const warning = findWarnings(this.response, attrs.node);
      const noDataWarning =
        this.response?.totalRowCount === 0
          ? new Error('Query returned no rows')
          : undefined;

      attrs.onQueryExecuted({
        columns: this.response.columns,
        error,
        warning,
        noDataWarning,
      });

      m.redraw();
    });
  }

  view({attrs}: m.CVnode<DataExplorerAttrs>) {
    const errors = findErrors(attrs.query, this.response);
    const statusText = this.getStatusText(attrs.query);
    const message = errors ? `Error: ${errors.message}` : statusText;

    return m(
      DetailsShell,
      {
        title: 'Query data',
        fillParent: true,
        buttons: this.renderMenu(attrs),
      },
      this.renderContent(attrs, message),
    );
  }

  private getStatusText(query?: Query | Error): string | undefined {
    if (query === undefined) {
      return 'No data to display';
    } else if (this.response === undefined) {
      return 'Typing...';
    }
    return undefined;
  }

  private renderMenu(attrs: DataExplorerAttrs): m.Children {
    const fullScreenButton = m(Button, {
      label: attrs.isFullScreen ? 'Exit full screen' : 'Full screen',
      onclick: () => attrs.onFullScreenToggle(),
    });

    if (attrs.isFullScreen) {
      return fullScreenButton;
    }

    const positionMenu = m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: Icons.ContextMenuAlt,
        }),
      },
      [
        m(MenuItem, {
          label: 'Left',
          onclick: () => attrs.onPositionChange('left'),
        }),
        m(MenuItem, {
          label: 'Right',
          onclick: () => attrs.onPositionChange('right'),
        }),
        m(MenuItem, {
          label: 'Bottom',
          onclick: () => attrs.onPositionChange('bottom'),
        }),
      ],
    );

    return [fullScreenButton, positionMenu];
  }

  private renderContent(
    attrs: DataExplorerAttrs,
    message?: string,
  ): m.Children {
    if (message) {
      return m(TextParagraph, {text: message});
    }

    if (this.response && this.dataSource) {
      const warning =
        this.response.statementWithOutputCount > 1
          ? m(
              Callout,
              {icon: 'warning'},
              `${this.response.statementWithOutputCount} out of ${this.response.statementCount} `,
              'statements returned a result. ',
              'Only the results for the last statement are displayed.',
            )
          : null;

      const maybeAggregateButton =
        attrs.isFullScreen &&
        attrs.node.type !== NodeType.kSqlSource &&
        m(
          '.pf-ndv-floating-button',
          m(Button, {
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            label: 'Aggregate',
            onclick: () => {
              this.showAggregationCard = !this.showAggregationCard;
            },
          }),
        );

      const maybeAggregationCard =
        this.showAggregationCard &&
        m(
          '.pf-ndv-floating-card',
          m(AggregationsOperator, {
            groupByColumns: attrs.node.state.groupByColumns,
            aggregations: attrs.node.state.aggregations,
          }),
        );

      const hasAggregations = (attrs.node.state.aggregations?.length ?? 0) > 0;
      const isSqlSource = attrs.node.type === NodeType.kSqlSource;

      return [
        warning,
        m(DataGrid, {
          fillHeight: true,
          columns: this.response.columns.map((c) => ({name: c})),
          data: this.dataSource,
          showFiltersInToolbar: true,
          filters: isSqlSource ? [] : attrs.node.state.filters,
          onFiltersChanged:
            hasAggregations || isSqlSource
              ? undefined
              : (filters: ReadonlyArray<FilterDefinition>) => {
                  attrs.node.state.filters = [...filters];
                  attrs.onchange?.();
                },
          cellRenderer: (value: SqlValue, name: string) => {
            return renderCell(value, name);
          },
        }),
        maybeAggregateButton,
        maybeAggregationCard,
      ];
    }
    return null;
  }
}

function findErrors(
  query?: Query | Error,
  response?: QueryResponse,
): Error | undefined {
  if (query instanceof Error) {
    return query;
  }
  if (response?.error) {
    return new Error(response.error);
  }
  return undefined;
}

function findWarnings(
  response: QueryResponse | undefined,
  node: QueryNode,
): Error | undefined {
  if (!response || response.error) {
    return undefined;
  }

  if (
    response.statementCount > 0 &&
    response.statementWithOutputCount === 0 &&
    response.columns.length === 0
  ) {
    return new Error('The last statement must produce an output.');
  }

  if (node.type === NodeType.kSqlSource && response.statementCount > 1) {
    const statements = response.query
      .split(';')
      .map((x) => x.trim())
      .filter((x) => x.length > 0);
    const allButLast = statements.slice(0, statements.length - 1);
    const moduleIncludeRegex = /^\s*INCLUDE\s+PERFETTO\s+MODULE\s+[\w._]+\s*$/i;
    for (const stmt of allButLast) {
      if (!moduleIncludeRegex.test(stmt)) {
        return new Error(
          `Only 'INCLUDE PERFETTO MODULE ...;' statements are ` +
            `allowed before the final statement. Error on: "${stmt}"`,
        );
      }
    }
  }

  return undefined;
}
