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
import {VirtualOverlayCanvas} from '../../../widgets/virtual_overlay_canvas';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderVirtualCanvas(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'VirtualCanvas'),
      m(
        'p',
        'A scrolling container that draws a virtual canvas overlay for rendering large amounts of graphical data efficiently.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: () => {
        const width = 200;
        const rowCount = 65536;
        const rowHeight = 20;
        return m(
          VirtualOverlayCanvas,
          {
            style: {height: '400px', width: `400px`},
            overflowY: 'auto',
            onCanvasRedraw({ctx, canvasRect}) {
              ctx.strokeStyle = 'red';
              ctx.lineWidth = 1;

              ctx.font = '20px Arial';
              ctx.fillStyle = 'black';
              ctx.textAlign = 'left';
              ctx.textBaseline = 'top';

              for (let i = 0; i < rowCount; i++) {
                const rect = {
                  left: 0,
                  top: i * rowHeight,
                  right: width,
                  bottom: i * rowHeight + rowHeight,
                };
                if (canvasRect.overlaps(rect)) {
                  ctx.strokeRect(0, i * rowHeight, width, rowHeight);
                  ctx.fillText(`Row: ${i}`, 0, i * rowHeight);
                }
              }
            },
          },
          m('', {
            style: {height: `${rowCount * rowHeight}px`, width: `${width}px`},
          }),
        );
      },
      initialOpts: {},
    }),
  ];
}
