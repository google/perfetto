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
import {shortUuid} from '../../../base/uuid';
import {Row} from './row';
import {Stack} from './stack';

export interface DraggableStackAttrs {
  readonly rows: readonly m.Children[];
  readonly onReorder: (from: number, to: number) => void;
  readonly onDelete: (index: number) => void;
  readonly deleteTitle?: string;
}

export class DraggableStack implements m.ClassComponent<DraggableStackAttrs> {
  private readonly mimeType = `application/x-pf-spag-${shortUuid().toLowerCase()}`;

  view({attrs}: m.CVnode<DraggableStackAttrs>): m.Children {
    const {rows, onReorder, onDelete, deleteTitle} = attrs;

    return m(
      Stack,
      rows.map((row, i) =>
        m(
          Row,
          {
            draggable: true,
            ondragstart: (e: DragEvent) => {
              if (!e.dataTransfer) return;
              e.dataTransfer.effectAllowed = 'move';
              e.dataTransfer.setData(this.mimeType, String(i));
              (e.currentTarget as HTMLElement).classList.add('pf-dragging');
            },
            ondragend: (e: DragEvent) => {
              (e.currentTarget as HTMLElement).classList.remove('pf-dragging');
            },
            ondragover: (e: DragEvent) => {
              if (!e.dataTransfer?.types.includes(this.mimeType)) return;
              e.preventDefault();
              e.dataTransfer.dropEffect = 'move';
              const el = e.currentTarget as HTMLElement;
              const rect = el.getBoundingClientRect();
              const isBottom = e.clientY > rect.top + rect.height / 2;
              el.classList.toggle('pf-drag-over-top', !isBottom);
              el.classList.toggle('pf-drag-over-bottom', isBottom);
            },
            ondragleave: (e: DragEvent) => {
              const el = e.currentTarget as HTMLElement;
              el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
            },
            ondrop: (e: DragEvent) => {
              const el = e.currentTarget as HTMLElement;
              const isBottom = el.classList.contains('pf-drag-over-bottom');
              el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
              if (!e.dataTransfer) return;
              const data = e.dataTransfer.getData(this.mimeType);
              if (data === '') return;
              e.preventDefault();
              const fromIdx = Number.parseInt(data, 10);
              const toIdx = isBottom ? i + 1 : i;
              if (
                !Number.isNaN(fromIdx) &&
                fromIdx >= 0 &&
                fromIdx < rows.length &&
                fromIdx !== toIdx &&
                fromIdx + 1 !== toIdx
              ) {
                onReorder(fromIdx, toIdx);
              }
            },
          },
          [
            m(Row.DragHandle),
            row,
            m(Row.DeleteButton, {
              tooltip: deleteTitle,
              onclick: () => onDelete(i),
            }),
          ],
        ),
      ),
    );
  }
}
