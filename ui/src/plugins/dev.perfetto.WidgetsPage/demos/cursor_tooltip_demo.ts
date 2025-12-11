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
import {CursorTooltip} from '../../../widgets/cursor_tooltip';
import {renderWidgetShowcase} from '../widgets_page_utils';

function CursorTooltipShowcase() {
  let show = false;
  return {
    view() {
      return m(
        '',
        {
          style: {
            width: '150px',
            height: '150px',
            border: '1px dashed gray',
            userSelect: 'none',
            color: 'gray',
            textAlign: 'center',
            lineHeight: '150px',
          },
          onmouseover: () => (show = true),
          onmouseout: () => (show = false),
        },
        'Hover here...',
        show && m(CursorTooltip, 'Hi!'),
      );
    },
  };
}

export function cursorTooltip(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'CursorTooltip'),
      m('p', 'A tooltip that follows the mouse around.'),
    ),
    renderWidgetShowcase({
      renderWidget: () => m(CursorTooltipShowcase),
    }),
  ];
}
