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
import {classNames} from '../base/classnames';
import {ButtonVariant} from './button';
import {CopyToClipboardButton} from './copy_to_clipboard_button';

interface CodeSnippetAttrs {
  // The text to be displayed in the code snippet.
  readonly text: string;
  // The language of the code snippet.
  readonly language?: string;
  // Any additional classes to apply to the container.
  readonly class?: string;
}

export class CodeSnippet implements m.ClassComponent<CodeSnippetAttrs> {
  view({attrs}: m.Vnode<CodeSnippetAttrs>) {
    const {text, language, class: className} = attrs;

    return m(
      '.pf-code-snippet',
      {
        className: classNames(className),
      },
      m(
        '.pf-code-snippet-header',
        m('span.pf-code-snippet-language', language),
        m(CopyToClipboardButton, {
          textToCopy: text,
          variant: ButtonVariant.Minimal,
          title: 'Copy to clipboard',
        }),
      ),
      m('pre', m('code', text)),
    );
  }
}
