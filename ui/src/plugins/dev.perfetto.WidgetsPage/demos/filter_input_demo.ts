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
import {
  FilterInput,
  TagDefinition,
  SelectedTag,
} from '../../../widgets/filter_input';
import {renderWidgetShowcase} from '../widgets_page_utils';

function FilterInputDemo() {
  const tags: TagDefinition[] = [
    {
      key: 'search',
      freeform: true,
      isDefault: true, // Default tag - used when typing without colon
    },
    {
      key: 'priority',
      values: [
        {key: 'high', label: 'High'},
        {key: 'medium', label: 'Medium'},
        {key: 'low', label: 'Low'},
      ],
    },
    {
      key: 'status',
      values: [
        {key: 'open', label: 'Open'},
        {key: 'in_progress', label: 'In Progress'},
        {key: 'done', label: 'Done'},
        {key: 'blocked', label: 'Blocked'},
      ],
    },
    {
      key: 'category',
      values: [
        {key: 'bug', label: 'Bug'},
        {key: 'feature', label: 'Feature'},
        {key: 'docs', label: 'Documentation'},
        {key: 'test', label: 'Test'},
      ],
    },
    {
      key: 'assignee',
      freeform: true, // Accepts any text
    },
  ];

  let selectedTags: SelectedTag[] = [];

  return {
    view: () => {
      return m(FilterInput, {
        tags,
        selectedTags,
        placeholder: 'Type to search, or tag:value (e.g., priority:high)',
        onTagAdd: (tag: SelectedTag) => {
          // Prevent duplicates
          const exists = selectedTags.some(
            (t) => t.tagKey === tag.tagKey && t.valueKey === tag.valueKey,
          );
          if (!exists) {
            selectedTags = [...selectedTags, tag];
          }
        },
        onTagRemove: (tag: SelectedTag) => {
          selectedTags = selectedTags.filter(
            (t) => !(t.tagKey === tag.tagKey && t.valueKey === tag.valueKey),
          );
        },
      });
    },
  };
}

export function filterInputDemo(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TagInput'),
      m(
        'p',
        'An input field for creating tag:value pairs with autocomplete. Features: (1) Type text to use the default "search" tag, (2) Type tag: to see available values for strict tags, (3) Type freeform_tag:any_text for freeform tags.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: () => m(FilterInputDemo),
      initialOpts: {},
    }),
  ];
}
