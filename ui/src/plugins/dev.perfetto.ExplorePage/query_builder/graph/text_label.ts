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
import {Label} from '../../../../widgets/nodegraph';

// Helper function to auto-resize textarea to fit content
function autoResizeTextarea(textarea: HTMLTextAreaElement) {
  textarea.style.height = 'auto';
  textarea.style.height = textarea.scrollHeight + 'px';
}

// Helper function to create labels with editable textarea content
// Double-click to start editing, blur to finish
export function createEditableTextLabels(
  labels: ReadonlyArray<Omit<Label, 'content'>>,
  labelTexts: Map<string, string>,
  editingLabels: Set<string>,
  onTextChange?: () => void,
): Label[] {
  return labels.map((label) => ({
    ...label,
    content: m('textarea.pf-text-label-textarea', {
      value: labelTexts.get(label.id) ?? '',
      readonly: !editingLabels.has(label.id),
      rows: 1,
      oncreate: (vnode: m.VnodeDOM) => {
        const textarea = vnode.dom as HTMLTextAreaElement;
        autoResizeTextarea(textarea);
      },
      onupdate: (vnode: m.VnodeDOM) => {
        const textarea = vnode.dom as HTMLTextAreaElement;
        autoResizeTextarea(textarea);
      },
      onpointerdown: (e: PointerEvent) => {
        const target = e.target as HTMLTextAreaElement;
        // If editing (not readonly), stop propagation to prevent dragging
        if (!target.readOnly) {
          e.stopPropagation();
        }
      },
      ondblclick: (e: Event) => {
        const target = e.target as HTMLTextAreaElement;
        editingLabels.add(label.id);
        target.focus();
        m.redraw();
      },
      oninput: (e: InputEvent) => {
        const target = e.target as HTMLTextAreaElement;
        labelTexts.set(label.id, target.value);
        autoResizeTextarea(target);
      },
      onblur: () => {
        editingLabels.delete(label.id);
        if (onTextChange) {
          onTextChange();
        }
        m.redraw();
      },
      onchange: (e: Event) => {
        const target = e.target as HTMLTextAreaElement;
        labelTexts.set(label.id, target.value);
        m.redraw();
      },
    }),
  }));
}
