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

import {Icons} from '../../../base/semantic_icons';
import {QueryResponse} from '../../../components/query_table/queries';
import {
  DataGridDataSource,
  FilterDefinition,
} from '../../../components/widgets/data_grid/common';
import {
  DataGrid,
  renderCell,
} from '../../../components/widgets/data_grid/data_grid';
import {SqlValue} from '../../../trace_processor/query_result';
import {Button} from '../../../widgets/button';
import {Callout} from '../../../widgets/callout';
import {DetailsShell} from '../../../widgets/details_shell';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {TextParagraph} from '../../../widgets/text_paragraph';
import {Query, QueryNode} from '../query_node';
import {QueryService} from './query_service';

import {findErrors} from './query_builder_utils';
export interface DataExplorerAttrs {
  readonly queryService: QueryService;
  readonly node: QueryNode;
  readonly query?: Query | Error;
  readonly executeQuery: boolean;
  readonly response?: QueryResponse;
  readonly dataSource?: DataGridDataSource;
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
          filters: attrs.node.state.filters,
          onFiltersChanged: (filters: ReadonlyArray<FilterDefinition>) => {
            attrs.node.state.filters = [...filters];
            attrs.onchange?.();
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
