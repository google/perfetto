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
import {oneDark} from '@codemirror/theme-one-dark';
import {keymap} from '@codemirror/view';
import {basicSetup, EditorView} from 'codemirror';
import m from 'mithril';
import {removeFalsyValues} from '../base/array_utils';
import {assertUnreachable} from '../base/logging';
import {perfettoSql} from '../base/perfetto_sql_lang/language';
import {HTMLAttrs} from './common';
import {classNames} from '../base/classnames';

export interface EditorAttrs extends HTMLAttrs {
  // Content of the editor. If defined, the editor operates in controlled mode,
  // otherwise it operates in uncontrolled mode.
  // - In controlled mode, the content of the editor is managed by the caller
  //   and should be used in conjunction with onUpdate to manage the state of
  //   the editor.
  // - In uncontrolled mode, the content of the editor is managed internally by
  //   the editor itself.
  readonly text?: string;

  // Which language use for syntax highlighting et al. Defaults to none.
  readonly language?: 'perfetto-sql';

  // Whether the editor should be focused on creation.
  readonly autofocus?: boolean;

  // Whether the editor should fill the height of its container.
  readonly fillHeight?: boolean;

  // Callback for the Ctrl/Cmd + Enter key binding.
  onExecute?: (text: string) => void;

  // Callback for the Ctrl/Cmd + S key binding.
  onSave?: () => void;

  // Callback for every change to the editor's content.
  onUpdate?: (text: string) => void;
}

export class Editor implements m.ClassComponent<EditorAttrs> {
  private editorView?: EditorView;
  private latestText?: string;

  focus() {
    this.editorView?.focus();
  }

  oncreate({dom, attrs}: m.CVnodeDOM<EditorAttrs>) {
    const keymaps = [indentWithTab];
    const onExecute = attrs.onExecute;
    const onSave = attrs.onSave;
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
          m.redraw();
          return true;
        },
      });
    }

    if (onSave) {
      keymaps.push({
        key: 'Mod-s',
        run: (_view: EditorView) => {
          onSave();
          m.redraw();
          return true;
        },
      });
    }

    const dispatch = (tr: Transaction, view: EditorView) => {
      // Maybe don't bother doing this if onUpdate is not defined...?
      view.update([tr]);
      const text = view.state.doc.toString();
      // Cache the latest text so that we don't immediately have to overwrite
      // this every time we make an edit to the doc if the caller just passes in
      // the exact same string again on the next redraw.
      this.latestText = text;

      if (onUpdate) {
        onUpdate(text);
        m.redraw();
      }
    };

    const lang = (() => {
      switch (attrs.language) {
        case undefined:
          return undefined;
        case 'perfetto-sql':
          return perfettoSql();
        default:
          assertUnreachable(attrs.language);
      }
    })();

    this.editorView = new EditorView({
      doc: attrs.text,
      extensions: removeFalsyValues([
        keymap.of(keymaps),
        oneDark,
        basicSetup,
        lang,
      ]),
      parent: dom,
      dispatch,
    });

    if (attrs.autofocus) {
      this.focus();
    }
  }

  onupdate({attrs}: m.CVnodeDOM<EditorAttrs>): void {
    // Uncontrolled mode: no need to do anything.
    if (attrs.text === undefined) {
      return;
    }

    const editorView = this.editorView;
    if (editorView && attrs.text !== this.latestText) {
      const state = editorView.state;
      editorView.dispatch(
        state.update({
          changes: {from: 0, to: state.doc.length, insert: attrs.text},
        }),
      );
      this.latestText = attrs.text;
    }
  }

  onremove(): void {
    if (this.editorView) {
      this.editorView.destroy();
      this.editorView = undefined;
    }
  }

  view({attrs}: m.Vnode<EditorAttrs>): m.Children {
    const className = classNames(
      attrs.className,
      attrs.fillHeight && 'pf-editor--fill-height',
    );
    return m('.pf-editor', {
      className: className,
      ref: attrs.ref,
    });
  }
}
