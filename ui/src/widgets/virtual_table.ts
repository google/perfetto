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
import {findRef, toHTMLElement} from '../base/dom_utils';
import {assertExists} from '../base/logging';
import {Style} from './common';
import {scheduleFullRedraw} from './raf';
import {VirtualScrollHelper} from './virtual_scroll_helper';
import {DisposableStack} from '../base/disposable_stack';

/**
 * The |VirtualTable| widget can be useful when attempting to render a large
 * amount of tabular data - i.e. dumping the entire contents of a database
 * table.
 *
 * A naive approach would be to load the entire dataset from the table and
 * render it into the DOM. However, this has a number of disadvantages:
 * - The query could potentially be very slow on large enough datasets.
 * - The amount of data pulled could be larger than the available memory.
 * - Rendering thousands of DOM elements using Mithril can get be slow.
 * - Asking the browser to create and update thousands of elements on the DOM
 *   can also be slow.
 *
 * This implementation takes advantage of the fact that computer monitors are
 * only so tall, so most will only be able to display a small subset of rows at
 * a given time, and the user will have to scroll to reveal more data.
 *
 * Thus, this widgets operates in such a way as to only render the DOM elements
 * that are visible within the given scrolling container's viewport. To avoid
 * spamming render updates, we render a few more rows above and below the
 * current viewport, and only trigger an update once the user scrolls too close
 * to the edge of the rendered data. These margins and tolerances are
 * configurable with the |renderOverdrawPx| and |renderTolerancePx| attributes.
 *
 * When it comes to loading data, it's often more performant to run fewer large
 * queries compared to more frequent smaller queries. Running a new query every
 * time we want to update the DOM is usually too frequent, and results in
 * flickering as the data is usually not loaded at the time the relevant row
 * scrolls into view.
 *
 * Thus, this implementation employs two sets of limits, one to refresh the DOM
 * and one larger one to re-query the data. The latter may be configured using
 * the |queryOverdrawPx| and |queryTolerancePx| attributes.
 *
 * The smaller DOM refreshes and handled internally, but the user must be called
 * to invoke a new query update. When new data is required, the |onReload|
 * callback is called with the row offset and count.
 *
 * The data must be passed in the |data| attribute which contains the offset of
 * the currently loaded data and a number of rows.
 *
 * Row and column content is flexible as m.Children are accepted and passed
 * straight to mithril.
 *
 * The widget is quite opinionated in terms of its styling, but the entire
 * widget and each row may be tweaked using |className| and |style| attributes
 * which behave in the same way as they do on other Mithril components.
 */

export interface VirtualTableAttrs {
  // A list of columns containing the header row content and column widths
  columns: VirtualTableColumn[];

  // Row height in px (each row must have the same height)
  rowHeight: number;

  // Offset of the first row
  firstRowOffset: number;

  // Total number of rows
  numRows: number;

  // The row data to render
  rows: VirtualTableRow[];

  // Optional: Called when we need to reload data
  onReload?: (rowOffset: number, rowCount: number) => void;

  // Additional class name applied to the table container element
  className?: string;

  // Additional styles applied to the table container element
  style?: Style;

  // Optional: Called when a row is hovered, passing the hovered row's id
  onRowHover?: (id: number) => void;

  // Optional: Called when a row is un-hovered, passing the un-hovered row's id
  onRowOut?: (id: number) => void;

  // Optional: Number of pixels equivalent of rows to overdraw above and below
  // the viewport
  // Defaults to a sensible value
  renderOverdrawPx?: number;

  // Optional: How close we can get to the edge before triggering a DOM redraw
  // Defaults to a sensible value
  renderTolerancePx?: number;

  // Optional: Number of pixels equivalent of rows to query above and below the
  // viewport
  // Defaults to a sensible value
  queryOverdrawPx?: number;

  // Optional: How close we can get to the edge if the loaded data before we
  // trigger another query
  // Defaults to a sensible value
  queryTolerancePx?: number;
}

export interface VirtualTableColumn {
  // Content to render in the header row
  header: m.Children;

  // CSS width e.g. 12px, 4em, etc...
  width: string;
}

export interface VirtualTableRow {
  // Id for this row (must be unique within this dataset)
  // Used for callbacks and as a Mithril key.
  id: number;

  // Data for each column in this row - must match number of elements in columns
  cells: m.Children[];

  // Optional: Additional class name applied to the row element
  className?: string;
}

export class VirtualTable implements m.ClassComponent<VirtualTableAttrs> {
  private readonly CONTAINER_REF = 'CONTAINER';
  private readonly SLIDER_REF = 'SLIDER';
  private readonly trash = new DisposableStack();
  private renderBounds = {rowStart: 0, rowEnd: 0};

  view({attrs}: m.Vnode<VirtualTableAttrs>): m.Children {
    const {columns, className, numRows, rowHeight, style} = attrs;
    return m(
      '.pf-vtable',
      {className, style, ref: this.CONTAINER_REF},
      m(
        '.pf-vtable-content',
        m(
          '.pf-vtable-header',
          columns.map((col) =>
            m('.pf-vtable-data', {style: {width: col.width}}, col.header),
          ),
        ),
        m(
          '.pf-vtable-slider',
          {ref: this.SLIDER_REF, style: {height: `${rowHeight * numRows}px`}},
          m(
            '.pf-vtable-puck',
            {
              style: {
                transform: `translateY(${
                  this.renderBounds.rowStart * rowHeight
                }px)`,
              },
            },
            this.renderContent(attrs),
          ),
        ),
      ),
    );
  }

  private renderContent(attrs: VirtualTableAttrs): m.Children {
    const rows: m.ChildArray = [];
    for (
      let i = this.renderBounds.rowStart;
      i < this.renderBounds.rowEnd;
      ++i
    ) {
      rows.push(this.renderRow(attrs, i));
    }
    return rows;
  }

  private renderRow(attrs: VirtualTableAttrs, i: number): m.Children {
    const {rows, firstRowOffset, rowHeight, columns, onRowHover, onRowOut} =
      attrs;
    if (i >= firstRowOffset && i < firstRowOffset + rows.length) {
      // Render the row...
      const index = i - firstRowOffset;
      const rowData = rows[index];
      return m(
        '.pf-vtable-row',
        {
          className: rowData.className,
          style: {height: `${rowHeight}px`},
          onmouseover: () => {
            onRowHover?.(rowData.id);
          },
          onmouseout: () => {
            onRowOut?.(rowData.id);
          },
        },
        rowData.cells.map((data, colIndex) =>
          m('.pf-vtable-data', {style: {width: columns[colIndex].width}}, data),
        ),
      );
    } else {
      // Render a placeholder div with the same height as a row but a
      // transparent background
      return m('', {style: {height: `${rowHeight}px`}});
    }
  }

  oncreate({dom, attrs}: m.VnodeDOM<VirtualTableAttrs>) {
    const {
      renderOverdrawPx = 200,
      renderTolerancePx = 100,
      queryOverdrawPx = 10_000,
      queryTolerancePx = 5_000,
    } = attrs;

    const sliderEl = toHTMLElement(assertExists(findRef(dom, this.SLIDER_REF)));
    const containerEl = assertExists(findRef(dom, this.CONTAINER_REF));
    const virtualScrollHelper = new VirtualScrollHelper(sliderEl, containerEl, [
      {
        overdrawPx: renderOverdrawPx,
        tolerancePx: renderTolerancePx,
        callback: (rect) => {
          const rowStart = Math.floor(rect.top / attrs.rowHeight / 2) * 2;
          const rowCount = Math.ceil(rect.height / attrs.rowHeight / 2) * 2;
          this.renderBounds = {rowStart, rowEnd: rowStart + rowCount};
          scheduleFullRedraw();
        },
      },
      {
        overdrawPx: queryOverdrawPx,
        tolerancePx: queryTolerancePx,
        callback: (rect) => {
          const rowStart = Math.floor(rect.top / attrs.rowHeight / 2) * 2;
          const rowEnd = Math.ceil(rect.bottom / attrs.rowHeight);
          attrs.onReload?.(rowStart, rowEnd - rowStart);
        },
      },
    ]);
    this.trash.use(virtualScrollHelper);
  }

  onremove(_: m.VnodeDOM<VirtualTableAttrs>) {
    this.trash.dispose();
  }
}
