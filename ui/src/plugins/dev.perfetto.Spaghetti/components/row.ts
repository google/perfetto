// Copyright (C) 2026 The Android Open Source Project
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
import {Icon} from '../../../widgets/icon';
import {Button} from '../../../widgets/button';
import './row.scss';

export interface RowReorder {
  // Index of this row within its list.
  readonly index: number;
  // Called after a drop with the source index and the destination index.
  // The destination index is already adjusted for the removal of the source
  // row, so callers can simply splice-remove `from` and splice-insert at `to`.
  readonly onMove: (from: number, to: number) => void;
}

export interface RowAttrs extends m.Attributes {
  // When set, the row renders a drag handle and supports drag reordering
  // within its list. While dragging, the source row is grayed out and the
  // sibling rows slide around to preview the resulting order.
  readonly reorder?: RowReorder;
}

// Returns a copy of `items` with the element at `from` moved to `to`.
// Matches the index semantics of RowReorder.onMove.
export function moveItem<T>(
  items: readonly T[],
  from: number,
  to: number,
): T[] {
  const updated = [...items];
  const [moved] = updated.splice(from, 1);
  updated.splice(to, 0, moved);
  return updated;
}

// State of the drag in progress. Drags are document-local so a single
// module-level slot is sufficient. The dragover/drop listeners live on the
// list container (the rows' parent element), which keeps hit-testing stable
// while the rows themselves are being translated around, and confines drops
// to the source list.
interface DragState {
  readonly from: number;
  readonly rows: HTMLElement[];
  // Height of one slot: source row height + list row-gap.
  readonly slotHeight: number;
  readonly onMove: (from: number, to: number) => void;
  readonly cleanup: () => void;
  // Current previewed destination index.
  preview: number;
}

let drag: DragState | null = null;

// Translate rows so the list previews the order after dropping at `to`.
function applyPreview(to: number) {
  if (drag === null || to === drag.preview) return;
  drag.preview = to;
  const {rows, from, slotHeight} = drag;
  rows.forEach((row, j) => {
    let shift = 0;
    if (j === from) {
      shift = (to - from) * slotHeight;
    } else if (from < to && j > from && j <= to) {
      shift = -slotHeight;
    } else if (to < from && j >= to && j < from) {
      shift = slotHeight;
    }
    row.style.transform = shift === 0 ? '' : `translateY(${shift}px)`;
  });
}

function startDrag(
  e: DragEvent,
  index: number,
  onMove: (from: number, to: number) => void,
) {
  const el = e.currentTarget as HTMLElement;
  const parent = el.parentElement;
  if (!parent) return;

  // Firefox requires setData for the drag to start at all.
  e.dataTransfer!.setData('text/plain', '');
  e.dataTransfer!.effectAllowed = 'move';
  el.classList.add('pf-dragging');

  const rows = Array.from(parent.children).filter((c): c is HTMLElement =>
    c.classList.contains('pf-spag-row'),
  );
  const gap = parseFloat(getComputedStyle(parent).rowGap) || 0;
  const slotHeight = el.offsetHeight + gap;

  const ondragover = (ev: DragEvent) => {
    ev.preventDefault();
    ev.dataTransfer!.dropEffect = 'move';
    const y = ev.clientY - parent.getBoundingClientRect().top;
    const to = Math.max(
      0,
      Math.min(rows.length - 1, Math.floor(y / slotHeight)),
    );
    applyPreview(to);
  };
  const ondrop = (ev: DragEvent) => {
    ev.preventDefault();
    if (drag !== null && drag.preview !== drag.from) {
      drag.onMove(drag.from, drag.preview);
      // These listeners are attached outside mithril, so redraw manually.
      m.redraw();
    }
  };
  parent.addEventListener('dragover', ondragover);
  parent.addEventListener('drop', ondrop);
  // Enables the transform transition on the rows for the duration of the
  // drag; removed before transforms are cleared so rows snap (not animate)
  // into their final rendered positions.
  parent.classList.add('pf-spag-drag-active');

  drag = {
    from: index,
    rows,
    slotHeight,
    onMove,
    preview: index,
    cleanup: () => {
      parent.classList.remove('pf-spag-drag-active');
      parent.removeEventListener('dragover', ondragover);
      parent.removeEventListener('drop', ondrop);
      for (const row of rows) {
        row.style.transform = '';
      }
    },
  };
}

export function Row(): m.Component<RowAttrs> {
  return {
    view({attrs, children}) {
      const {reorder, ...rest} = attrs;
      if (!reorder) {
        return m('.pf-spag-row', rest, children);
      }

      const {index, onMove} = reorder;
      return m(
        '.pf-spag-row.pf-spag-row--reorderable',
        {
          ...rest,
          ondragstart: (e: DragEvent) => {
            startDrag(e, index, onMove);
          },
          ondragend: (e: DragEvent) => {
            const el = e.currentTarget as HTMLElement;
            el.classList.remove('pf-dragging');
            el.removeAttribute('draggable');
            drag?.cleanup();
            drag = null;
          },
        },
        m(Icon, {
          icon: 'drag_indicator',
          className: 'pf-spag-draghandle',
          title: 'Drag to reorder',
          // Only drags initiated from the handle should move the row;
          // dragging inside text inputs must keep selecting text. The
          // draggable attribute is toggled directly on the DOM node so it
          // takes effect before the browser decides to start a drag.
          onmousedown: (e: MouseEvent) => {
            const row = (e.currentTarget as HTMLElement).closest(
              '.pf-spag-row',
            );
            if (!(row instanceof HTMLElement)) return;
            row.setAttribute('draggable', 'true');
            document.addEventListener(
              'mouseup',
              () => row.removeAttribute('draggable'),
              {once: true},
            );
          },
        }),
        children,
      );
    },
  };
}

export namespace Row {
  export interface DeleteButtonAttrs {
    readonly onclick?: () => void;
    readonly title?: string;
  }

  export const DeleteButton: m.Component<DeleteButtonAttrs> = {
    view({attrs}) {
      return m(Button, {
        icon: 'close',
        className: 'pf-spag-delete',
        title: attrs.title ?? 'Remove',
        onclick: attrs.onclick,
      });
    },
  };
}
