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
  EditorState,
  StateEffect,
  StateField,
  type Transaction,
} from '@codemirror/state';
import {oneDark} from '@codemirror/theme-one-dark';
import {
  Decoration,
  type DecorationSet,
  hoverTooltip,
  keymap,
  ViewPlugin,
  type ViewUpdate,
} from '@codemirror/view';
import {basicSetup, EditorView} from 'codemirror';
import {javascript} from '@codemirror/lang-javascript';
import m from 'mithril';
import {removeFalsyValues} from '../base/array_utils';
import {assertUnreachable} from '../base/assert';
import {perfettoSql} from '../base/perfetto_sql_lang/language';
import type {HTMLAttrs} from './common';
import {classNames} from '../base/classnames';

type EditorLanguage = 'perfetto-sql' | 'javascript';

// Minimal structural mirror of CodeMirror's autocomplete API, so callers can
// supply a completion source without depending on @codemirror/autocomplete
// directly (basicSetup already runs the autocompletion extension; we just feed
// it a source via languageData).
export interface CompletionContextLike {
  readonly pos: number;
  readonly explicit: boolean;
  readonly state: EditorState;
  matchBefore(expr: RegExp): {from: number; to: number; text: string} | null;
}
export interface CompletionOption {
  readonly label: string;
  readonly type?: string;
  readonly detail?: string;
  readonly info?: string;
  readonly apply?: string;
  readonly boost?: number;
}
export interface CompletionResultLike {
  readonly from: number;
  readonly to?: number;
  readonly options: ReadonlyArray<CompletionOption>;
  readonly validFor?: RegExp;
}
export type EditorCompletionSource = (
  ctx: CompletionContextLike,
) => CompletionResultLike | null;

// Structural mirror of a diagnostic, so callers can drive inline error/warning
// underlines without depending on @codemirror/lint (which isn't a direct dep).
// Offsets are document positions (UTF-16 code units, which match byte offsets
// for the ASCII that SQL is in practice).
export interface EditorDiagnostic {
  readonly from: number;
  readonly to: number;
  readonly severity: 'error' | 'warning' | 'info' | 'hint';
  readonly message: string;
  // Optional secondary line (e.g. a "did you mean …" hint) shown in the hover.
  readonly help?: string;
}
export type EditorDiagnosticSource = (
  text: string,
) => ReadonlyArray<EditorDiagnostic>;

// Effect dispatched to force a diagnostics re-run even without a doc edit (used
// when the caller's async backing — e.g. a WASM engine — becomes ready).
const refreshDiagnostics = StateEffect.define<null>();

interface DiagnosticsState {
  readonly decorations: DecorationSet;
  readonly diags: ReadonlyArray<EditorDiagnostic>;
}

function computeDiagnostics(
  source: EditorDiagnosticSource,
  state: EditorState,
): DiagnosticsState {
  const len = state.doc.length;
  const diags: EditorDiagnostic[] = [];
  const ranges = [];
  for (const d of source(state.doc.toString())) {
    const from = Math.max(0, Math.min(d.from, len));
    const to = Math.max(from, Math.min(d.to, len));
    diags.push(d);
    // A zero-width range can't carry a mark decoration; skip the underline but
    // still keep the diagnostic so the hover tooltip can surface it.
    if (to > from) {
      ranges.push(
        Decoration.mark({
          class: `pf-cm-diag pf-cm-diag--${d.severity}`,
        }).range(from, to),
      );
    }
  }
  return {decorations: Decoration.set(ranges, true), diags};
}

// How long after the last edit to recompute diagnostics. Recomputing on every
// keystroke would run the (synchronous) parser on the whole document each time;
// debouncing keeps typing smooth. Mirrors @codemirror/lint's default cadence.
const DIAGNOSTICS_DEBOUNCE_MS = 250;

// Builds the CodeMirror extension that renders `source`'s diagnostics as inline
// underlines plus a hover tooltip (message + optional help). The recompute is
// debounced: on each edit the existing decorations are mapped through the change
// (so they keep tracking the text), and the full recompute runs once typing
// pauses — or immediately when forced via refreshDiagnostics.
//
// Why not @codemirror/lint's linter()? It isn't a direct dependency — it's only
// present transitively (basicSetup pulls in its lintKeymap), and pnpm's strict
// node_modules makes transitive-only packages non-importable, so using it would
// mean adding a dep + regenerating the frozen lockfile. This is a small,
// self-contained reimplementation on @codemirror/view/state (both direct deps);
// the forced-refresh-on-engine-ready path (refreshDiagnostics) is also simpler
// to express here than via linter()'s needsRefresh facet.
function buildDiagnosticsExtension(source: EditorDiagnosticSource) {
  const field = StateField.define<DiagnosticsState>({
    create: (state) => computeDiagnostics(source, state),
    update: (value, tr) => {
      if (tr.effects.some((e) => e.is(refreshDiagnostics))) {
        return computeDiagnostics(source, tr.state);
      }
      if (tr.docChanged) {
        // Keep existing underlines tracking the edit until the debounced
        // recompute lands.
        return {
          decorations: value.decorations.map(tr.changes),
          diags: value.diags.map((d) => ({
            ...d,
            from: tr.changes.mapPos(d.from),
            to: tr.changes.mapPos(d.to),
          })),
        };
      }
      return value;
    },
    provide: (f) => EditorView.decorations.from(f, (v) => v.decorations),
  });

  // Debounces the full recompute: schedules a forced refresh once edits settle.
  const debouncer = ViewPlugin.fromClass(
    class {
      private timer = 0;
      constructor(private readonly view: EditorView) {}
      update(u: ViewUpdate) {
        if (!u.docChanged) return;
        clearTimeout(this.timer);
        this.timer = window.setTimeout(
          () => this.view.dispatch({effects: refreshDiagnostics.of(null)}),
          DIAGNOSTICS_DEBOUNCE_MS,
        );
      }
      destroy() {
        clearTimeout(this.timer);
      }
    },
  );

  const tooltip = hoverTooltip((view, pos) => {
    const {diags} = view.state.field(field);
    const hit = diags.find((d) => pos >= d.from && pos <= d.to);
    if (!hit) return null;
    return {
      pos: hit.from,
      end: hit.to,
      above: true,
      create: () => {
        const dom = document.createElement('div');
        dom.className = 'pf-cm-diag-tooltip';
        const msg = document.createElement('div');
        msg.className = 'pf-cm-diag-tooltip__msg';
        msg.textContent = hit.message;
        dom.appendChild(msg);
        if (hit.help) {
          const help = document.createElement('div');
          help.className = 'pf-cm-diag-tooltip__help';
          help.textContent = hit.help;
          dom.appendChild(help);
        }
        return {dom};
      },
    };
  });

  return [field, debouncer, tooltip];
}

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

  // Optional autocomplete source (e.g. schema-aware SQL completion). Fed into
  // the autocompletion extension that basicSetup already provides.
  readonly completions?: EditorCompletionSource;

  // Optional diagnostics source (e.g. parser-grade SQL errors). Rendered as
  // inline underlines + a hover tooltip. Re-run (debounced) after edits.
  readonly diagnostics?: EditorDiagnosticSource;

  // Called once on create with a `refresh` function that re-runs the
  // diagnostics source. Lets the caller refresh when its async backing (e.g. a
  // WASM engine) becomes ready, so squiggles appear without waiting for an edit.
  onDiagnosticsRefresh?(refresh: () => void): void;
}

export class Editor implements m.ClassComponent<EditorAttrs> {
  private editorView?: EditorView;
  private latestText?: string;

  focus() {
    this.editorView?.focus();
  }

  oncreate({dom, attrs}: m.CVnodeDOM<EditorAttrs>) {
    this.latestText = attrs.text;
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

    // Completion + diagnostics are only installed when the caller supplies them
    // (e.g. via the SqlEditorIntelligence plugin); otherwise the editor carries
    // neither, so this is inert for every other consumer.
    const completions = attrs.completions;
    const completion = completions
      ? EditorState.languageData.of(() => [{autocomplete: completions}])
      : undefined;
    const diagnostics = attrs.diagnostics
      ? buildDiagnosticsExtension(attrs.diagnostics)
      : undefined;

    this.editorView = new EditorView({
      doc: attrs.text,
      extensions: removeFalsyValues([
        keymap.of(keymaps),
        ...readonly,
        oneDark,
        basicSetup,
        lang,
        completion,
        diagnostics,
      ]),
      parent: dom,
      dispatch,
    });

    if (diagnostics && attrs.onDiagnosticsRefresh) {
      attrs.onDiagnosticsRefresh(() =>
        this.editorView?.dispatch({effects: refreshDiagnostics.of(null)}),
      );
    }

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
