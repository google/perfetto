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
import {Button} from '../../../widgets/button';
import {EmptyState} from '../../../widgets/empty_state';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderEmptyState(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'EmptyState'),
      m(
        'p',
        'A placeholder component for displaying helpful messages and actions when there is no content to show.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({header, content}) =>
        m(
          EmptyState,
          {
            title: header ? 'No search results found...' : undefined,
          },
          content && m(Button, {label: 'Try again'}),
        ),
      initialOpts: {
        header: true,
        content: true,
      },
    }),
  ];
}
