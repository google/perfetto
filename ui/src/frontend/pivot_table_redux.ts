/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import * as m from 'mithril';
import {sqliteString} from '../base/string_utils';
import {Actions, DeferredAction} from '../common/actions';
import {ColumnType} from '../common/query_result';
import {
  Area,
  PivotTableReduxAreaState,
  PivotTableReduxQuery,
  PivotTableReduxResult
} from '../common/state';
import {PivotTree} from '../controller/pivot_table_redux_controller';

import {globals} from './globals';
import {Panel} from './panel';
import {
  aggregationIndex,
  areaFilter,
  generateQuery,
  QueryGeneratorError,
  sliceAggregationColumns,
  Table,
  TableColumn,
  tables,
  threadSliceAggregationColumns
} from './pivot_table_redux_query_generator';

interface PathItem {
  tree: PivotTree;
  nextKey: ColumnType;
}

// Used to convert TableColumn to a string in order to store it in a Map, as
// ES6 does not support compound Set/Map keys.
export function columnKey(tableColumn: TableColumn): string {
  return `${tableColumn[0]}.${tableColumn[1]}`;
}

// Arguments to an action to toggle a table column in a particular part of
// application's state.
interface ColumnSetArgs {
  column: TableColumn;
  selected: boolean;
}

interface ColumnSetCheckboxAttrs {
  set: (args: ColumnSetArgs) => DeferredAction<ColumnSetArgs>;
  get: Map<string, TableColumn>;
  setKey: TableColumn;
}

// Helper component that controls whether a particular key is present in a
// ColumnSet.
class ColumnSetCheckbox implements m.ClassComponent<ColumnSetCheckboxAttrs> {
  view({attrs}: m.Vnode<ColumnSetCheckboxAttrs>) {
    return m('input[type=checkbox]', {
      onclick: (e: InputEvent) => {
        const target = e.target as HTMLInputElement;

        globals.dispatch(
            attrs.set({column: attrs.setKey, selected: target.checked}));
        globals.rafScheduler.scheduleFullRedraw();
      },
      checked: attrs.get.has(columnKey(attrs.setKey))
    });
  }
}

interface PivotTableReduxAttrs {
  selectionArea: PivotTableReduxAreaState;
}

interface DrillFilter {
  column: string;
  value: ColumnType;
}

// Convert DrillFilter to SQL condition to be used in WHERE clause.
function renderDrillFilter(filter: DrillFilter): string {
  if (filter.value === null) {
    return `${filter.column} IS NULL`;
  } else if (typeof filter.value === 'number') {
    return `${filter.column} = ${filter.value}`;
  }
  return `${filter.column} = ${sqliteString(filter.value)}`;
}

export class PivotTableRedux extends Panel<PivotTableReduxAttrs> {
  get selectedPivotsMap() {
    return globals.state.nonSerializableState.pivotTableRedux.selectedPivotsMap;
  }

  get selectedAggregations() {
    return globals.state.nonSerializableState.pivotTableRedux
        .selectedAggregations;
  }

  get constrainToArea() {
    return globals.state.nonSerializableState.pivotTableRedux.constrainToArea;
  }

  renderCanvas(): void {}

  generateQuery(attrs: PivotTableReduxAttrs): PivotTableReduxQuery {
    return generateQuery(
        this.selectedPivotsMap,
        this.selectedAggregations,
        globals.state.areas[attrs.selectionArea.areaId],
        this.constrainToArea);
  }

  renderTablePivotColumns(t: Table) {
    return m(
        'li',
        t.name,
        m('ul',
          t.columns.map(
              col =>
                  m('li',
                    m(ColumnSetCheckbox, {
                      get: this.selectedPivotsMap,
                      set: Actions.setPivotTablePivotSelected,
                      setKey: [t.name, col],
                    }),
                    col))));
  }

  renderResultsView(attrs: PivotTableReduxAttrs) {
    return m(
        '.pivot-table-redux',
        m('button.mode-button',
          {
            onclick: () => {
              globals.dispatch(Actions.setPivotTableEditMode({editMode: true}));
              globals.rafScheduler.scheduleFullRedraw();
            }
          },
          'Edit'),
        this.renderResultsTable(attrs));
  }

  renderDrillDownCell(
      area: Area, result: PivotTableReduxResult, filters: DrillFilter[]) {
    return m(
        'td',
        m('button',
          {
            title: 'All corresponding slices',
            onclick: () => {
              const queryFilters = filters.map(renderDrillFilter);
              if (this.constrainToArea) {
                queryFilters.push(areaFilter(area));
              }
              const query = `
                select * from ${result.metadata.tableName}
                where ${queryFilters.join(' and \n')}
              `;
              // TODO(ddrone): the UI of running query as if it was a canned or
              // custom query is a temporary one, replace with a proper UI.
              globals.dispatch(Actions.executeQuery({
                queryId: 'command',
                query,
              }));
            }
          },
          m('i.material-icons', 'arrow_right')));
  }

  renderSectionRow(
      area: Area, path: PathItem[], tree: PivotTree,
      result: PivotTableReduxResult): m.Vnode {
    const renderedCells = [];
    for (let j = 0; j + 1 < path.length; j++) {
      renderedCells.push(m('td', m('span.indent', ' '), `${path[j].nextKey}`));
    }

    const treeDepth = result.metadata.pivotColumns.length;
    const colspan = treeDepth - path.length + 1;
    const button =
        m('button',
          {
            onclick: () => {
              tree.isCollapsed = !tree.isCollapsed;
              globals.rafScheduler.scheduleFullRedraw();
            }
          },
          m('i.material-icons',
            tree.isCollapsed ? 'expand_more' : 'expand_less'));

    renderedCells.push(
        m('td', {colspan}, button, `${path[path.length - 1].nextKey}`));

    for (const value of tree.aggregates) {
      renderedCells.push(m('td', `${value}`));
    }

    const drillFilters: DrillFilter[] = [];
    for (let i = 0; i < path.length; i++) {
      drillFilters.push({
        value: `${path[i].nextKey}`,
        column: result.metadata.pivotColumns[i]
      });
    }

    renderedCells.push(this.renderDrillDownCell(area, result, drillFilters));
    return m('tr', renderedCells);
  }

  renderTree(
      area: Area, path: PathItem[], tree: PivotTree,
      result: PivotTableReduxResult, sink: m.Vnode[]) {
    if (tree.isCollapsed) {
      sink.push(this.renderSectionRow(area, path, tree, result));
      return;
    }
    if (tree.children.size > 0) {
      // Avoid rendering the intermediate results row for the root of tree
      // and in case there's only one child subtree.
      if (!tree.isCollapsed && path.length > 0 && tree.children.size !== 1) {
        sink.push(this.renderSectionRow(area, path, tree, result));
      }
      for (const [key, childTree] of tree.children.entries()) {
        path.push({tree: childTree, nextKey: key});
        this.renderTree(area, path, childTree, result, sink);
        path.pop();
      }
      return;
    }

    // Avoid rendering the intermediate results row if it has only one leaf
    // row.
    if (!tree.isCollapsed && path.length > 0 && tree.rows.length > 1) {
      sink.push(this.renderSectionRow(area, path, tree, result));
    }
    for (const row of tree.rows) {
      const renderedCells = [];
      const drillFilters: DrillFilter[] = [];
      const treeDepth = result.metadata.pivotColumns.length;
      for (let j = 0; j < treeDepth; j++) {
        if (j < path.length) {
          renderedCells.push(m('td', m('span.indent', ' '), `${row[j]}`));
        } else {
          renderedCells.push(m(`td`, `${row[j]}`));
        }
        drillFilters.push(
            {column: result.metadata.pivotColumns[j], value: row[j]});
      }
      for (let j = 0; j < result.metadata.aggregationColumns.length; j++) {
        const value = row[aggregationIndex(treeDepth, j, treeDepth)];
        renderedCells.push(m('td', `${value}`));
      }

      renderedCells.push(this.renderDrillDownCell(area, result, drillFilters));
      sink.push(m('tr', renderedCells));
    }
  }

  renderTotalsRow(queryResult: PivotTableReduxResult) {
    const overallValuesRow =
        [m('td.total-values',
           {'colspan': queryResult.metadata.pivotColumns.length},
           m('strong', 'Total values:'))];
    for (const aggValue of queryResult.tree.aggregates) {
      overallValuesRow.push(m('td', `${aggValue}`));
    }
    overallValuesRow.push(m('td'));
    return m('tr', overallValuesRow);
  }

  renderResultsTable(attrs: PivotTableReduxAttrs) {
    const state = globals.state.nonSerializableState.pivotTableRedux;
    if (state.queryResult === null) {
      return m('div', 'Loading...');
    }

    const renderedRows: m.Vnode[] = [];
    const tree = state.queryResult.tree;

    if (tree.children.size === 0 && tree.rows.length === 0) {
      // Empty result, render a special message
      return m('.empty-result', 'No slices in the current selection.');
    }

    this.renderTree(
        globals.state.areas[attrs.selectionArea.areaId],
        [],
        tree,
        state.queryResult,
        renderedRows);

    const allColumns = state.queryResult.metadata.pivotColumns.concat(
        state.queryResult.metadata.aggregationColumns);
    return m(
        'table.query-table.pivot-table',
        m('thead', m('tr', allColumns.map(column => m('td', column)), m('td'))),
        m('tbody', this.renderTotalsRow(state.queryResult), renderedRows));
  }

  renderQuery(attrs: PivotTableReduxAttrs): m.Vnode {
    // Prepare a button to switch to results mode.
    let innerElement = m(
        'button.mode-button',
        {
          onclick: () => {
            globals.dispatch(Actions.setPivotTableEditMode({editMode: false}));
            globals.rafScheduler.scheduleFullRedraw();
          }
        },
        'Execute');
    try {
      this.generateQuery(attrs);
    } catch (e) {
      if (e instanceof QueryGeneratorError) {
        // If query generation fails, show an error message instead of a button.
        innerElement = m('div.query-error', e.message);
      } else {
        throw e;
      }
    }

    return m(
        'div',
        m('div',
          m('input', {
            type: 'checkbox',
            id: 'constrain-to-selection',
            checked: this.constrainToArea,
            onclick: (e: InputEvent) => {
              const checkbox = e.target as HTMLInputElement;
              globals.dispatch(Actions.setPivotTableReduxConstrainToArea(
                  {constrain: checkbox.checked}));
            }
          }),
          m('label',
            {
              'for': 'constrain-to-selection',
            },
            'Constrain to current time range')),
        innerElement);
  }

  view({attrs}: m.Vnode<PivotTableReduxAttrs>) {
    return globals.state.nonSerializableState.pivotTableRedux.editMode ?
        this.renderEditView(attrs) :
        this.renderResultsView(attrs);
  }

  renderEditView(attrs: PivotTableReduxAttrs) {
    return m(
        '.pivot-table-redux.edit',
        m('div',
          m('h2', 'Pivots'),
          m('ul',
            tables.map(
                t => this.renderTablePivotColumns(t),
                ))),
        m('div',
          m('h2', 'Aggregations'),
          m('ul',
            ...sliceAggregationColumns.map(
                t =>
                    m('li',
                      m(ColumnSetCheckbox, {
                        get: this.selectedAggregations,
                        set: Actions.setPivotTableAggregationSelected,
                        setKey: ['slice', t],
                      }),
                      t)),
            ...threadSliceAggregationColumns.map(
                t =>
                    m('li',
                      m(ColumnSetCheckbox, {
                        get: this.selectedAggregations,
                        set: Actions.setPivotTableAggregationSelected,
                        setKey: ['thread_slice', t],
                      }),
                      `thread_slice.${t}`)))),
        this.renderQuery(attrs));
  }
}