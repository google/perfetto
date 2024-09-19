// Copyright (C) 2023 The Android Open Source Project
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

import {indentWithTab} from '@codemirror/commands';
import {Transaction} from '@codemirror/state';
import {oneDarkTheme} from '@codemirror/theme-one-dark';
import {keymap} from '@codemirror/view';
import {basicSetup, EditorView} from 'codemirror';
import m from 'mithril';
import {assertExists} from '../base/logging';
import {DragGestureHandler} from '../base/drag_gesture_handler';
import {DisposableStack} from '../base/disposable_stack';

export interface EditorAttrs {
  // Initial state for the editor.
  initialText?: string;

  // Changing generation is used to force resetting of the editor state
  // to the current value of initialText.
  generation?: number;

  // Callback for the Ctrl/Cmd + Enter key binding.
  onExecute?: (text: string) => void;

  // Callback for every change to the text.
  onUpdate?: (text: string) => void;
}

export class Editor implements m.ClassComponent<EditorAttrs> {
  private editorView?: EditorView;
  private generation?: number;
  private trash = new DisposableStack();

  oncreate({dom, attrs}: m.CVnodeDOM<EditorAttrs>) {
    const keymaps = [indentWithTab];
    const onExecute = attrs.onExecute;
    const onUpdate = attrs.onUpdate;

    if (onExecute) {
      keymaps.push({
        key: 'Mod-Enter',
        run: (view: EditorView) => {
          const state = view.state;
          const selection = state.selection;
          let text = state.doc.toString();
          if (!selection.main.empty) {
            let selectedText = '';

            for (const r of selection.ranges) {
              selectedText += text.slice(r.from, r.to);
            }

            text = selectedText;
          }
          onExecute(text);
          return true;
        },
      });
    }

    let dispatch;
    if (onUpdate) {
      dispatch = (tr: Transaction, view: EditorView) => {
        view.update([tr]);
        const text = view.state.doc.toString();
        onUpdate(text);
      };
    }

    this.generation = attrs.generation;

    this.editorView = new EditorView({
      doc: attrs.initialText ?? '',
      extensions: [keymap.of(keymaps), oneDarkTheme, basicSetup],
      parent: dom,
      dispatch,
    });

    // Install the drag handler for the resize bar.
    let initialH = 0;
    this.trash.use(
      new DragGestureHandler(
        assertExists(dom.querySelector('.resize-handler')) as HTMLElement,
        /* onDrag */
        (_x, y) => ((dom as HTMLElement).style.height = `${initialH + y}px`),
        /* onDragStarted */
        () => (initialH = dom.clientHeight),
        /* onDragFinished */
        () => {},
      ),
    );
  }

  onupdate({attrs}: m.CVnodeDOM<EditorAttrs>): void {
    const {initialText, generation} = attrs;
    const editorView = this.editorView;
    if (editorView && this.generation !== generation) {
      const state = editorView.state;
      editorView.dispatch(
        state.update({
          changes: {from: 0, to: state.doc.length, insert: initialText},
        }),
      );
      this.generation = generation;
    }
  }

  onremove(): void {
    if (this.editorView) {
      this.editorView.destroy();
      this.editorView = undefined;
    }
    this.trash.dispose();
  }

  view({}: m.Vnode<EditorAttrs, this>): void | m.Children {
    return m('.pf-editor', m('.resize-handler'));
  }
}
