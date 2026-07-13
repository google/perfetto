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

import './editor.scss';
import {indentWithTab} from '@codemirror/commands';
import {
  Compartment,
  EditorState,
  type Extension,
  type Transaction,
} from '@codemirror/state';
import {oneDark} from '@codemirror/theme-one-dark';
import {keymap, tooltips} from '@codemirror/view';
import {basicSetup, EditorView} from 'codemirror';
import {javascript} from '@codemirror/lang-javascript';
import m from 'mithril';
import {removeFalsyValues} from '../base/array_utils';
import {assertUnreachable} from '../base/assert';
import {perfettoSql} from '../base/perfetto_sql_lang/language';
import type {HTMLAttrs} from './common';
import {classNames} from '../base/classnames';

type EditorLanguage = 'perfetto-sql' | 'javascript';

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
  readonly language?: EditorLanguage;

  // Whether the editor should be focused on creation.
  readonly autofocus?: boolean;

  // Whether the editor should fill the height of its container.
  readonly fillHeight?: boolean;

  // Whether the editor content is readonly.
  readonly readonly?: boolean;

  // Callback for the Ctrl/Cmd + Enter key binding.
  onExecute?: (text: string) => void;

  // Callback for the Ctrl/Cmd + S key binding.
  onSave?: () => void;

  // Callback for the Alt/Opt + Shift + F key binding.
  onFormat?: (text: string) => void;

  // Callback for every change to the editor's content.
  onUpdate?: (text: string) => void;

  // Extra CodeMirror extensions supplied by the caller (e.g. the LSP
  // integration from the SqlLsp plugin).
  readonly extensions?: Extension;
}

export class Editor implements m.ClassComponent<EditorAttrs> {
  private editorView?: EditorView;
  private latestText?: string;
  // Caller extensions live in a compartment so they can be swapped in on a
  // later render (e.g. a plugin registering them after this editor mounted).
  private readonly callerExtensions = new Compartment();
  private latestExtensions?: Extension;

  focus() {
    this.editorView?.focus();
  }

  oncreate({dom, attrs}: m.CVnodeDOM<EditorAttrs>) {
    this.latestText = attrs.text;
    this.latestExtensions = attrs.extensions;
    const keymaps = [indentWithTab];
    const onExecute = attrs.onExecute;
    const onSave = attrs.onSave;
    const onFormat = attrs.onFormat;
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

    if (onFormat) {
      keymaps.push({
        key: 'Alt-Shift-f',
        run: (view: EditorView) => {
          onFormat(view.state.doc.toString());
          m.redraw();
          return true;
        },
      });
    }

    const dispatch = (tr: Transaction, view: EditorView) => {
      view.update([tr]);
      const text = view.state.doc.toString();
      // Only fire onUpdate when text actually changes, not for cursor
      // movements, selection changes, or other non-text transactions.
      if (onUpdate && text !== this.latestText) {
        this.latestText = text;
        onUpdate(text);
        m.redraw();
      } else {
        this.latestText = text;
      }
    };

    const lang = (() => {
      switch (attrs.language) {
        case undefined:
          return undefined;
        case 'perfetto-sql':
          return perfettoSql();
        case 'javascript':
          return javascript();
        default:
          assertUnreachable(attrs.language);
      }
    })();

    const readonly = (() => {
      if (attrs.readonly) {
        return [
          EditorState.readOnly.of(true),
          EditorView.editable.of(false),
          // Enable keyboard commands by allowing focus.
          EditorView.contentAttributes.of({tabindex: '0'}),
        ];
      }
      return [];
    })();

    this.editorView = new EditorView({
      doc: attrs.text,
      extensions: removeFalsyValues([
        keymap.of(keymaps),
        ...readonly,
        oneDark,
        basicSetup,
        lang,
        // Float popups (autocomplete, hover tooltips) in a body portal so an
        // `overflow: hidden` ancestor (e.g. the query page's split pane)
        // doesn't clip them at the pane edge.
        tooltips({parent: document.body, position: 'fixed'}),
        this.callerExtensions.of(attrs.extensions ?? []),
      ]),
      parent: dom,
      dispatch,
    });

    if (attrs.autofocus) {
      this.focus();
    }
  }

  onupdate({attrs}: m.CVnodeDOM<EditorAttrs>): void {
    if (attrs.extensions !== this.latestExtensions) {
      this.latestExtensions = attrs.extensions;
      this.editorView?.dispatch({
        effects: this.callerExtensions.reconfigure(attrs.extensions ?? []),
      });
    }

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
