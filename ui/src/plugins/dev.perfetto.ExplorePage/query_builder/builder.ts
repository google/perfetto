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

import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode} from '../query_node';
import {Query, QueryNodeExplorer} from './query_node_explorer';
import {QueryCanvas} from './query_canvas';
import {Trace} from 'src/public/trace';
import {NodeDataViewer} from './node_data_viewer';

export interface QueryBuilderAttrs {
  readonly trace: Trace;

  readonly sqlModules: SqlModules;
  readonly rootNodes: QueryNode[];
  readonly selectedNode?: QueryNode;

  readonly onRootNodeCreated: (node: QueryNode) => void;
  readonly onNodeSelected: (node?: QueryNode) => void;
  readonly renderNodeActionsMenuItems: (node: QueryNode) => m.Children;
  readonly addSourcePopupMenu: () => m.Children;
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  private query?: Query;
  private queryExecuted: boolean = false;
  private tablePosition: 'left' | 'right' | 'bottom' = 'right';

  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
    const {
      trace,
      rootNodes,
      onNodeSelected,
      selectedNode,
      renderNodeActionsMenuItems,
      addSourcePopupMenu,
    } = attrs;

    console.log('Table position:', this.tablePosition);

    const canvasStyle: m.Attributes['style'] = {
      gridColumn: 1,
      gridRow: 1,
      overflow: 'auto',
    };
    const explorerStyle: m.Attributes['style'] = {
      gridColumn: 2,
      gridRow: 1,
      overflow: 'auto',
    };
    const viewerStyle: m.Attributes['style'] = {
      gridColumn: 2,
      gridRow: 2,
      overflow: 'auto',
    };

    switch (this.tablePosition) {
      case 'left':
        viewerStyle.gridColumn = 1;
        explorerStyle.gridRow = '1/span 2';
        break;
      case 'right':
        viewerStyle.gridColumn = 2;
        canvasStyle.gridRow = '1/ span 2';
        break;
      case 'bottom':
        viewerStyle.gridColumn = '1/span 2';
        break;
    }

    return m(
      '.query-builder-layout',
      {
        style: {
          display: 'grid',
          gridTemplateColumns: '50% 50%',
          gridTemplateRows: '50% 50%',
          gap: '10px',
          height: '100%',
        },
      },
      m(
        '',
        {style: canvasStyle},
        m(QueryCanvas, {
          rootNodes,
          selectedNode,
          onNodeSelected,
          renderNodeActionsMenuItems,
          addSourcePopupMenu,
        }),
      ),
      attrs.selectedNode &&
        m(
          '',
          {style: explorerStyle},
          m(QueryNodeExplorer, {
            trace,
            node: attrs.selectedNode,
            onQueryAnalyzed: (query: Query) => {
              this.query = query;
              this.queryExecuted = false;
            },
          }),
        ),
      attrs.selectedNode &&
        m(
          '',
          {style: viewerStyle},
          m(NodeDataViewer, {
            trace,
            query: this.query,
            executeQuery: !this.queryExecuted,
            onQueryExecuted: () => {
              this.queryExecuted = true;
            },
            onPositionChange: (pos: 'left' | 'right' | 'bottom') => {
              this.tablePosition = pos;
            },
          }),
        ),
    );
  }
}
