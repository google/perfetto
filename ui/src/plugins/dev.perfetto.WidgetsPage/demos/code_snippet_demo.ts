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
import {CodeSnippet} from '../../../widgets/code_snippet';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderCodeSnippet(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'CodeSnippet'),
      m(
        'p',
        'A syntax-highlighted code block component for displaying code examples with language-specific formatting.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({wide}) =>
        m(CodeSnippet, {
          language: 'SQL',
          text: wide
            ? 'SELECT a_very_long_column_name, another_super_long_column_name, yet_another_ridiculously_long_column_name FROM a_table_with_an_unnecessarily_long_name WHERE some_condition_is_true AND another_condition_is_also_true;'
            : 'SELECT * FROM slice LIMIT 10;',
        }),
      initialOpts: {
        wide: false,
      },
    }),
  ];
}
