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
import {TagInput} from '../../../widgets/tag_input';
import {renderWidgetShowcase} from '../widgets_page_utils';

function TagInputDemo() {
  const tags: string[] = ['foo', 'bar', 'baz'];
  let tagInputValue: string = '';

  return {
    view: () => {
      return m(TagInput, {
        tags,
        value: tagInputValue,
        onTagAdd: (tag) => {
          tags.push(tag);
          tagInputValue = '';
        },
        onChange: (value) => {
          tagInputValue = value;
        },
        onTagRemove: (index) => {
          tags.splice(index, 1);
        },
      });
    },
  };
}

export function renderTagInput(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TagInput'),
      m(
        'p',
        'An input field for managing multiple tags or values, with support for adding and removing items.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: () => m(TagInputDemo),
      initialOpts: {},
    }),
  ];
}
