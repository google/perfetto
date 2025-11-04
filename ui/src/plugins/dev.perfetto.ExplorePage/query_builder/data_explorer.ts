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

import {QueryResponse} from '../../../components/query_table/queries';
import {
  DataGridDataSource,
  FilterNull,
  FilterValue,
} from '../../../components/widgets/data_grid/common';
import {
  DataGrid,
  renderCell,
} from '../../../components/widgets/data_grid/data_grid';
import {SqlValue} from '../../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Callout} from '../../../widgets/callout';
import {DetailsShell} from '../../../widgets/details_shell';
import {Spinner} from '../../../widgets/spinner';
import {Switch} from '../../../widgets/switch';
import {TextParagraph} from '../../../widgets/text_paragraph';
import {Query, QueryNode, isAQuery} from '../query_node';
import {QueryService} from './query_service';
import {Intent} from '../../../widgets/common';
import {Icons} from '../../../base/semantic_icons';
import {MenuItem, PopupMenu} from '../../../widgets/menu';

import {findErrors} from './query_builder_utils';
export interface DataExplorerAttrs {
  readonly queryService: QueryService;
  readonly node: QueryNode;
  readonly query?: Query | Error;
  readonly response?: QueryResponse;
  readonly dataSource?: DataGridDataSource;
  readonly isQueryRunning: boolean;
  readonly isFullScreen: boolean;
  readonly onFullScreenToggle: () => void;
  readonly onExecute: () => void;
  readonly onchange?: () => void;
}

export class DataExplorer implements m.ClassComponent<DataExplorerAttrs> {
  view({attrs}: m.CVnode<DataExplorerAttrs>) {
    const errors = findErrors(attrs.query, attrs.response);
    const statusText = this.getStatusText(attrs.query, attrs.response);
    const message = errors ? `Error: ${errors.message}` : statusText;

    return m(
      DetailsShell,
      {
        title: 'Query data',
        fillHeight: true,
        buttons: this.renderMenu(attrs),
      },
      this.renderContent(attrs, message),
    );
  }

  private getStatusText(
    query?: Query | Error,
    response?: QueryResponse,
  ): string | undefined {
    if (query === undefined) {
      return 'No data to display';
    } else if (response === undefined) {
      return 'Typing...';
    }
    return undefined;
  }

  private renderMenu(attrs: DataExplorerAttrs): m.Children {
    const autoExecute = attrs.node.state.autoExecute ?? true;

    // Show spinner only when a query is actually running
    // Don't show spinner before user clicks "Run Query" button
    const isUpdating = attrs.isQueryRunning;

    const runButton =
      !autoExecute &&
      m(Button, {
        label: 'Run Query',
        icon: 'play_arrow',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        disabled: !isAQuery(attrs.query) || !attrs.node.validate(),
        onclick: () => attrs.onExecute(),
      });

    const spinner = isUpdating && m(Spinner);

    const autoExecuteSwitch = m(Switch, {
      label: 'Auto Execute',
      checked: autoExecute,
      onchange: (e: Event) => {
        const target = e.target as HTMLInputElement;
        attrs.node.state.autoExecute = target.checked;
        attrs.onchange?.();
      },
    });

    const positionMenu = m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: Icons.ContextMenuAlt,
        }),
      },
      [
        m(MenuItem, {
          label: attrs.isFullScreen ? 'Exit full screen' : 'Full screen',
          onclick: () => attrs.onFullScreenToggle(),
        }),
      ],
    );

    return [runButton, spinner, autoExecuteSwitch, positionMenu];
  }

  private renderContent(
    attrs: DataExplorerAttrs,
    message?: string,
  ): m.Children {
    // Show validation errors as callouts
    if (!attrs.node.validate() && attrs.node.state.issues?.queryError) {
      return m(
        Callout,
        {icon: 'info'},
        attrs.node.state.issues.queryError.message,
      );
    }

    if (message) {
      return m(TextParagraph, {text: message});
    }

    if (attrs.response && attrs.dataSource && attrs.node.validate()) {
      const warning =
        attrs.response.statementWithOutputCount > 1
          ? m(
              Callout,
              {icon: 'warning'},
              `${attrs.response.statementWithOutputCount} out of ${attrs.response.statementCount} `,
              'statements returned a result. ',
              'Only the results for the last statement are displayed.',
            )
          : null;

      return [
        warning,
        m(DataGrid, {
          fillHeight: true,
          columns: attrs.response.columns.map((c) => ({name: c})),
          data: attrs.dataSource,
          showFiltersInToolbar: true,
          // We don't actually want the datagrid to display or apply any filters
          // to the datasource itself, so we define this but fix it as an empty
          // array.
          filters: [],
          onFilterAdd: (filter) => {
            // These are the filters supported by the explore page currently.
            const supportedOps = [
              '=',
              '!=',
              '<',
              '<=',
              '>',
              '>=',
              'glob',
              'is null',
              'is not null',
            ];
            if (supportedOps.includes(filter.op)) {
              attrs.node.state.filters = [
                ...(attrs.node.state.filters ?? []),
                filter as FilterValue | FilterNull,
              ];
              attrs.onchange?.();
            }
          },
          cellRenderer: (value: SqlValue, name: string) => {
            return renderCell(value, name);
          },
        }),
      ];
    }
    return null;
  }
}
