// Copyright (C) 2022 The Android Open Source Project
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
import {DropDirection} from '../core/pivot_table_manager';
import {raf} from '../core/raf_scheduler';

export interface ReorderableCell {
  content: m.Children;
  extraClass?: string;
}

export interface ReorderableCellGroupAttrs {
  cells: ReorderableCell[];
  onReorder: (from: number, to: number, side: DropDirection) => void;
}

const placeholderElement = document.createElement('span');

// A component that renders a group of cells on the same row that can be
// reordered between each other by using drag'n'drop.
//
// On completed reorder, a callback is fired.
export class ReorderableCellGroup
  implements m.ClassComponent<ReorderableCellGroupAttrs>
{
  // Index of a cell being dragged.
  draggingFrom: number = -1;

  // Index of a cell cursor is hovering over.
  draggingTo: number = -1;

  // Whether the cursor hovering on the left or right side of the element: used
  // to add the dragged element either before or after the drop target.
  dropDirection: DropDirection = 'left';

  // Auxillary array used to count entrances into `dragenter` event: these are
  // incremented not only when hovering over a cell, but also for any child of
  // the tree.
  enterCounters: number[] = [];

  getClassForIndex(index: number): string {
    if (this.draggingFrom === index) {
      return 'dragged';
    }
    if (this.draggingTo === index) {
      return this.dropDirection === 'left'
        ? 'highlight-left'
        : 'highlight-right';
    }
    return '';
  }

  view(vnode: m.Vnode<ReorderableCellGroupAttrs>): m.Children {
    return vnode.attrs.cells.map((cell, index) =>
      m(
        `td.reorderable-cell${cell.extraClass ?? ''}`,
        {
          draggable: 'draggable',
          class: this.getClassForIndex(index),
          ondragstart: (e: DragEvent) => {
            this.draggingFrom = index;
            if (e.dataTransfer !== null) {
              e.dataTransfer.setDragImage(placeholderElement, 0, 0);
            }

            raf.scheduleFullRedraw();
          },
          ondragover: (e: DragEvent) => {
            let target = e.target as HTMLElement;
            if (this.draggingFrom === index || this.draggingFrom === -1) {
              // Don't do anything when hovering on the same cell that's
              // been dragged, or when dragging something other than the
              // cell from the same group
              return;
            }

            while (
              target.tagName.toLowerCase() !== 'td' &&
              target.parentElement !== null
            ) {
              target = target.parentElement;
            }

            // When hovering over cell on the right half, the cell will be
            // moved to the right of it, vice versa for the left side. This
            // is done such that it's possible to put dragged cell to every
            // possible position.
            const offset = e.clientX - target.getBoundingClientRect().x;
            const newDropDirection =
              offset > target.clientWidth / 2 ? 'right' : 'left';
            const redraw =
              newDropDirection !== this.dropDirection ||
              index !== this.draggingTo;
            this.dropDirection = newDropDirection;
            this.draggingTo = index;

            if (redraw) {
              raf.scheduleFullRedraw();
            }
          },
          ondragenter: (e: DragEvent) => {
            this.enterCounters[index]++;

            if (this.enterCounters[index] === 1 && e.dataTransfer !== null) {
              e.dataTransfer.dropEffect = 'move';
            }
          },
          ondragleave: (e: DragEvent) => {
            this.enterCounters[index]--;
            if (this.draggingFrom === -1 || this.enterCounters[index] > 0) {
              return;
            }

            if (e.dataTransfer !== null) {
              e.dataTransfer.dropEffect = 'none';
            }

            this.draggingTo = -1;
            raf.scheduleFullRedraw();
          },
          ondragend: () => {
            if (
              this.draggingTo !== this.draggingFrom &&
              this.draggingTo !== -1
            ) {
              vnode.attrs.onReorder(
                this.draggingFrom,
                this.draggingTo,
                this.dropDirection,
              );
            }

            this.draggingFrom = -1;
            this.draggingTo = -1;
            raf.scheduleFullRedraw();
          },
        },
        cell.content,
      ),
    );
  }

  oncreate(vnode: m.VnodeDOM<ReorderableCellGroupAttrs, this>) {
    this.enterCounters = Array(vnode.attrs.cells.length).fill(0);
  }

  onupdate(vnode: m.VnodeDOM<ReorderableCellGroupAttrs, this>) {
    if (this.enterCounters.length !== vnode.attrs.cells.length) {
      this.enterCounters = Array(vnode.attrs.cells.length).fill(0);
    }
  }
}
