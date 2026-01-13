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
import {parseAndPrintTree} from '../../../base/perfetto_sql_lang/language';
import {Editor} from '../../../widgets/editor';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';
import {CodeSnippet} from '../../../widgets/code_snippet';

interface EditorDemoState {
  text: string;
}

class EditorDemo implements m.ClassComponent {
  private state: EditorDemoState = {
    text: 'SELECT * FROM slice;',
  };

  view() {
    return [
      m(
        '.pf-widget-intro',
        m('h1', 'Editor'),
        m('p', [
          'A code editor component with syntax highlighting, powered by CodeMirror. ',
          'Supports basic Perfetto SQL syntax highlighting.',
        ]),
      ),
      renderWidgetShowcase({
        renderWidget: ({language, readonly, fillHeight}) =>
          m(Editor, {
            key: `${language}-${readonly}`,
            language: language == 'perfetto-sql' ? 'perfetto-sql' : undefined,
            fillHeight,
            readonly,
            text: this.state.text,
            onUpdate: (text) => {
              parseAndPrintTree(text);
              this.state.text = text;
            },
          }),
        initialOpts: {
          language: new EnumOption('perfetto-sql', ['perfetto-sql', 'none']),
          readonly: true,
          fillHeight: true,
        },
      }),

      m('.pf-widget-doc-section', [
        m('h2', 'Basic Usage'),
        m(
          'p',
          m(CodeSnippet, {
            text: `m(Editor, {
  language: 'perfetto-sql',
  fillHeight: true,
  readonly: false,
  initialText: 'SELECT * FROM slice',
  onUpdate: (text) => {
    // Handle text changes
    console.log('Editor content:', text);
  },
})`,
            language: 'typescript',
          }),
        ),
      ]),

      m('.pf-widget-doc-section', [
        m('h2', 'Key Features'),
        m('ul', [
          m('li', [
            m('strong', 'Syntax Highlighting: '),
            'Language-specific syntax highlighting for Perfetto SQL',
          ]),
          m('li', [
            m('strong', 'Keyboard Shortcuts: '),
            'Standard editor shortcuts (Ctrl+Z for undo, Ctrl+F for find, Ctrl+Enter to execute, etc.)',
          ]),
        ]),
      ]),
    ];
  }
}

export function renderEditor(): m.Children {
  return m(EditorDemo);
}
