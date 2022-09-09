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
import {Actions} from '../common/actions';
import {COUNT_AGGREGATION} from '../common/empty_state';
import {ColumnType} from '../common/query_result';
import {
  Area,
  PivotTableReduxAreaState,
  PivotTableReduxQuery,
  PivotTableReduxResult,
  SortDirection,
} from '../common/state';
import {fromNs, timeToCode} from '../common/time';
import {
  PivotTableReduxController,
} from '../controller/pivot_table_redux_controller';

import {globals} from './globals';
import {fullscreenModalContainer, ModalDefinition} from './modal';
import {Panel} from './panel';
import {AnyAttrsVnode} from './panel_container';
import {ArgumentPopup} from './pivot_table_redux_argument_popup';
import {
  aggregationIndex,
  areaFilter,
  extractArgumentExpression,
  generateQuery,
  sliceAggregationColumns,
  tables,
} from './pivot_table_redux_query_generator';
import {
  Aggregation,
  AggregationFunction,
  columnKey,
  PivotTree,
  TableColumn,
  tableColumnEquals,
} from './pivot_table_redux_types';
import {PopupMenuButton, PopupMenuItem} from './popup_menu';
import {DropDirection, ReorderableCellGroup} from './reorderable_cells';


interface PathItem {
  tree: PivotTree;
  nextKey: ColumnType;
}

interface PivotTableReduxAttrs {
  selectionArea: PivotTableReduxAreaState;
}

interface DrillFilter {
  column: TableColumn;
  value: ColumnType;
}

function drillFilterColumnName(column: TableColumn): string {
  switch (column.kind) {
    case 'argument':
      return extractArgumentExpression(column.argument);
    case 'regular':
      return `${column.table}.${column.column}`;
  }
}

// Convert DrillFilter to SQL condition to be used in WHERE clause.
function renderDrillFilter(filter: DrillFilter): string {
  const column = drillFilterColumnName(filter.column);
  if (filter.value === null) {
    return `${column} IS NULL`;
  } else if (typeof filter.value === 'number') {
    return `${column} = ${filter.value}`;
  }
  return `${column} = ${sqliteString(filter.value)}`;
}

function readableColumnName(column: TableColumn) {
  switch (column.kind) {
    case 'argument':
      return `Argument ${column.argument}`;
    case 'regular':
      return `${column.table}.${column.column}`;
  }
}

export class PivotTableRedux extends Panel<PivotTableReduxAttrs> {
  get pivotState() {
    return globals.state.nonSerializableState.pivotTableRedux;
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
        this.pivotState.selectedPivots,
        this.pivotState.selectedSlicePivots,
        this.selectedAggregations,
        globals.state.areas[attrs.selectionArea.areaId],
        this.constrainToArea);
  }

  renderDrillDownCell(area: Area, filters: DrillFilter[]) {
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
                select slice.* from slice
                join thread_track on slice.track_id = thread_track.id
                join thread using (utid)
                join process using (upid)
                where ${queryFilters.join(' and \n')}
              `;
              // TODO(ddrone): the UI of running query as if it was a canned or
              // custom query is a temporary one, replace with a proper UI.
              globals.dispatch(Actions.executeQuery({
                queryId: `pivot_table_details_${
                    PivotTableReduxController.detailsCount++}`,
                query,
              }));
            },
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
            },
          },
          m('i.material-icons',
            tree.isCollapsed ? 'expand_more' : 'expand_less'));

    renderedCells.push(
        m('td', {colspan}, button, `${path[path.length - 1].nextKey}`));

    for (let i = 0; i < tree.aggregates.length; i++) {
      const renderedValue = this.renderCell(
          result.metadata.aggregationColumns[i].column, tree.aggregates[i]);
      renderedCells.push(m('td', renderedValue));
    }

    const drillFilters: DrillFilter[] = [];
    for (let i = 0; i < path.length; i++) {
      drillFilters.push({
        value: `${path[i].nextKey}`,
        column: result.metadata.pivotColumns[i],
      });
    }

    renderedCells.push(this.renderDrillDownCell(area, drillFilters));
    return m('tr', renderedCells);
  }

  renderCell(column: TableColumn, value: ColumnType): string {
    if (column.kind === 'regular' &&
        (column.column === 'dur' || column.column === 'thread_dur')) {
      if (typeof value === 'number') {
        return timeToCode(fromNs(value));
      }
    }
    return `${value}`;
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
        const value = this.renderCell(result.metadata.pivotColumns[j], row[j]);
        if (j < path.length) {
          renderedCells.push(m('td', m('span.indent', ' '), value));
        } else {
          renderedCells.push(m(`td`, value));
        }
        drillFilters.push(
            {column: result.metadata.pivotColumns[j], value: row[j]});
      }
      for (let j = 0; j < result.metadata.aggregationColumns.length; j++) {
        const value = row[aggregationIndex(treeDepth, j, treeDepth)];
        const renderedValue = this.renderCell(
            result.metadata.aggregationColumns[j].column, value);
        renderedCells.push(m('td', renderedValue));
      }

      renderedCells.push(this.renderDrillDownCell(area, drillFilters));
      sink.push(m('tr', renderedCells));
    }
  }

  renderTotalsRow(queryResult: PivotTableReduxResult) {
    const overallValuesRow =
        [m('td.total-values',
           {'colspan': queryResult.metadata.pivotColumns.length},
           m('strong', 'Total values:'))];
    for (let i = 0; i < queryResult.tree.aggregates.length; i++) {
      overallValuesRow.push(
          m('td',
            this.renderCell(
                queryResult.metadata.aggregationColumns[i].column,
                queryResult.tree.aggregates[i])));
    }
    overallValuesRow.push(m('td'));
    return m('tr', overallValuesRow);
  }

  sortingItem(column: TableColumn, order: SortDirection): PopupMenuItem {
    return {
      itemType: 'regular',
      text: order === 'DESC' ? 'Highest first' : 'Lowest first',
      callback() {
        globals.dispatch(Actions.setPivotTableSortColumn({column, order}));
        globals.dispatch(
            Actions.setPivotTableQueryRequested({queryRequested: true}));
      },
    };
  }

  readableAggregationName(aggregation: Aggregation) {
    if (aggregation.aggregationFunction === 'COUNT') {
      return 'Count';
    }
    return `${aggregation.aggregationFunction}(${
        readableColumnName(aggregation.column)})`;
  }

  aggregationPopupItem(aggregation: Aggregation, nameOverride?: string):
      PopupMenuItem {
    return {
      itemType: 'regular',
      text: nameOverride ?? readableColumnName(aggregation.column),
      callback: () => {
        globals.dispatch(Actions.setPivotTableAggregationSelected({
          column: {
            aggregationFunction: aggregation.aggregationFunction,
            column: aggregation.column,
          },
          selected: true,
        }));
        globals.dispatch(
            Actions.setPivotTableQueryRequested({queryRequested: true}));
      },
    };
  }

  aggregationPopupTableGroup(
      table: string, columns: string[], used: Set<string>): PopupMenuItem
      |undefined {
    const items = [];
    for (const column of columns) {
      const tableColumn: TableColumn = {kind: 'regular', table, column};
      if (used.has(columnKey(tableColumn))) {
        continue;
      }

      items.push(this.aggregationPopupItem(
          {aggregationFunction: 'SUM', column: tableColumn}));
    }

    if (items.length === 0) {
      return undefined;
    }

    return {
      itemType: 'group',
      itemId: `aggregations-${table}`,
      text: `Add ${table} aggregation`,
      children: items,
    };
  }

  renderAggregationHeaderCell(aggregation: Aggregation, removeItem: boolean):
      m.Child {
    const column = aggregation.column;
    const popupItems: PopupMenuItem[] = [];
    const state = globals.state.nonSerializableState.pivotTableRedux;
    let icon = 'more_horiz';
    if (state.sortCriteria === undefined ||
        !tableColumnEquals(column, state.sortCriteria.column)) {
      popupItems.push(
          this.sortingItem(column, 'DESC'), this.sortingItem(column, 'ASC'));
    } else {
      // Table is already sorted by the same column, return one item with
      // opposite direction.
      popupItems.push(this.sortingItem(
          column, state.sortCriteria.order === 'DESC' ? 'ASC' : 'DESC'));
      icon = state.sortCriteria.order === 'DESC' ? 'arrow_drop_down' :
                                                   'arrow_drop_up';
    }
    const otherAggs: AggregationFunction[] = ['SUM', 'MAX', 'MIN'];
    if (aggregation.aggregationFunction !== 'COUNT') {
      for (const otherAgg of otherAggs) {
        if (aggregation.aggregationFunction === otherAgg) {
          continue;
        }

        popupItems.push({
          itemType: 'regular',
          text: otherAgg,
          callback() {
            globals.dispatch(Actions.setPivotTableAggregationSelected(
                {column: aggregation, selected: false}));
            globals.dispatch(Actions.setPivotTableAggregationSelected({
              column:
                  {aggregationFunction: otherAgg, column: aggregation.column},
              selected: true,
            }));
            globals.dispatch(
                Actions.setPivotTableQueryRequested({queryRequested: true}));
          },
        });
      }
    }

    if (removeItem) {
      popupItems.push({
        itemType: 'regular',
        text: 'Remove',
        callback() {
          globals.dispatch(Actions.setPivotTableAggregationSelected(
              {column: aggregation, selected: false}));
          globals.dispatch(
              Actions.setPivotTableQueryRequested({queryRequested: true}));
        },
      });
    }

    const usedAggregations: Set<string> = new Set();
    let hasCount = false;

    for (const agg of state.selectedAggregations.values()) {
      if (agg.aggregationFunction === 'COUNT') {
        hasCount = true;
        continue;
      }

      usedAggregations.add(columnKey(agg.column));
    }

    if (!hasCount) {
      popupItems.push(this.aggregationPopupItem(
          COUNT_AGGREGATION, 'Add count aggregation'));
    }

    const sliceAggregationsItem = this.aggregationPopupTableGroup(
        'slice', sliceAggregationColumns, usedAggregations);
    if (sliceAggregationsItem !== undefined) {
      popupItems.push(sliceAggregationsItem);
    }

    return m(
        'td', this.readableAggregationName(aggregation), m(PopupMenuButton, {
          icon,
          items: popupItems,
        }));
  }

  showModal = false;
  typedArgument = '';

  renderModal(): ModalDefinition {
    return {
      title: 'Enter argument name',
      content: m(ArgumentPopup, {
                 knownArguments: globals.state.nonSerializableState
                                     .pivotTableRedux.argumentNames,
                 onArgumentChange: (arg) => {
                   this.typedArgument = arg;
                 },
               }) as AnyAttrsVnode,
      buttons: [
        {
          text: 'Add',
          action: () => {
            globals.dispatch(Actions.setPivotTablePivotSelected({
              column: {kind: 'argument', argument: this.typedArgument},
              selected: true,
            }));
            globals.dispatch(
                Actions.setPivotTableQueryRequested({queryRequested: true}));
          },
        },
      ],
    };
  }

  renderPivotColumnHeader(
      queryResult: PivotTableReduxResult, pivot: TableColumn,
      selectedPivots: Set<string>): m.Children {
    const items: PopupMenuItem[] = [{
      itemType: 'regular',
      text: 'Add argument pivot',
      callback: () => {
        this.showModal = true;
        this.typedArgument = '';
        fullscreenModalContainer.createNew(this.renderModal());
      },
    }];
    if (queryResult.metadata.pivotColumns.length > 1) {
      items.push({
        itemType: 'regular',
        text: 'Remove',
        callback() {
          globals.dispatch(Actions.setPivotTablePivotSelected(
              {column: pivot, selected: false}));
          globals.dispatch(
              Actions.setPivotTableQueryRequested({queryRequested: true}));
        },
      });
    }

    for (const table of tables) {
      const group: PopupMenuItem[] = [];
      for (const columnName of table.columns) {
        const column: TableColumn = {
          kind: 'regular',
          table: table.name,
          column: columnName,
        };
        if (selectedPivots.has(columnKey(column))) {
          continue;
        }

        group.push({
          itemType: 'regular',
          text: columnName,
          callback() {
            globals.dispatch(
                Actions.setPivotTablePivotSelected({column, selected: true}));
            globals.dispatch(
                Actions.setPivotTableQueryRequested({queryRequested: true}));
          },
        });
      }
      items.push({
        itemType: 'group',
        itemId: `pivot-${table.name}`,
        text: `Add ${table.name} pivot`,
        children: group,
      });
    }

    return [
      readableColumnName(pivot),
      m(PopupMenuButton, {icon: 'more_horiz', items}),
    ];
  }

  renderResultsTable(attrs: PivotTableReduxAttrs) {
    const state = globals.state.nonSerializableState.pivotTableRedux;
    if (state.queryResult === null) {
      return m('div', 'Loading...');
    }
    const queryResult: PivotTableReduxResult = state.queryResult;

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

    const selectedPivots = new Set([
      ...this.pivotState.selectedPivots,
      ...this.pivotState.selectedSlicePivots,
    ].map((pivot) => columnKey(pivot)));
    const pivotTableHeaders = state.selectedPivots.map(
        (pivot) =>
            this.renderPivotColumnHeader(queryResult, pivot, selectedPivots));
    const slicePivotTableHeaders = state.selectedSlicePivots.map(
        (pivot) =>
            this.renderPivotColumnHeader(queryResult, pivot, selectedPivots));

    const removeItem = state.queryResult.metadata.aggregationColumns.length > 1;
    const aggregationTableHeaders =
        state.queryResult.metadata.aggregationColumns.map(
            (aggregation) =>
                this.renderAggregationHeaderCell(aggregation, removeItem));

    return m(
        'table.query-table.pivot-table',
        m('thead',
          // First row of the table, containing names of pivot and aggregation
          // columns, as well as popup menus to modify the columns. Last cell
          // is empty because of an extra column with "drill down" button for
          // each pivot table row.
          m('tr',
            m(ReorderableCellGroup, {
              cells: pivotTableHeaders,
              onReorder: (
                  from: number, to: number, direction: DropDirection) => {
                globals.dispatch(
                    Actions.changePivotTablePivotOrder({from, to, direction}));
                globals.dispatch(Actions.setPivotTableQueryRequested(
                    {queryRequested: true}));
              },
            }),
            m(ReorderableCellGroup, {
              cells: slicePivotTableHeaders,
              onReorder:
                  (from: number, to: number, direction: DropDirection) => {
                    globals.dispatch(Actions.changePivotTableSlicePivotOrder(
                        {from, to, direction}));
                    globals.dispatch(Actions.setPivotTableQueryRequested(
                        {queryRequested: true}));
                  },
            }),
            aggregationTableHeaders,
            m('td.menu', m(PopupMenuButton, {
                icon: 'menu',
                items: [{
                  itemType: 'regular',
                  text: state.constrainToArea ?
                      'Query data for the whole timeline' :
                      'Constrain to selected area',
                  callback: () => {
                    globals.dispatch(Actions.setPivotTableReduxConstrainToArea(
                        {constrain: !state.constrainToArea}));
                    globals.dispatch(Actions.setPivotTableQueryRequested(
                        {queryRequested: true}));
                  },
                }],
              })))),
        m('tbody', this.renderTotalsRow(state.queryResult), renderedRows));
  }

  view({attrs}: m.Vnode<PivotTableReduxAttrs>): m.Children {
    if (this.showModal) {
      fullscreenModalContainer.updateVdom(this.renderModal());
    }

    return m('.pivot-table-redux', this.renderResultsTable(attrs));
  }
}
