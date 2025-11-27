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
import {ResizeHandle} from '../../../widgets/resize_handle';
import {CodeSnippet} from '../../../widgets/code_snippet';

interface ResizeHandleDemoState {
  verticalHeight: number;
  horizontalWidth: number;
}

class ResizeHandleDemo implements m.ClassComponent {
  private state: ResizeHandleDemoState = {
    verticalHeight: 200,
    horizontalWidth: 300,
  };

  view(): m.Children {
    return [
      m(
        '.pf-widget-intro',
        m('h1', 'ResizeHandle'),
        m('p', [
          'A draggable handle for resizing panels and containers. ',
          'Supports both vertical (default) and horizontal orientations.',
        ]),
      ),

      m('.pf-widget-doc-section', [
        m('h2', 'Vertical ResizeHandle (Default)'),
        m('p', 'Drag the handle to resize the panel vertically:'),
        m(
          '.pf-resize-demo-container',
          {
            style: {
              border: '1px solid var(--pf-color-border)',
              borderRadius: '4px',
              overflow: 'hidden',
              marginBottom: '16px',
            },
          },
          [
            m(
              '.pf-resize-demo-content',
              {
                style: {
                  height: `${this.state.verticalHeight}px`,
                  background: 'var(--pf-color-background-secondary)',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  padding: '16px',
                },
              },
              `Height: ${Math.round(this.state.verticalHeight)}px (drag handle below to resize)`,
            ),
            m(ResizeHandle, {
              onResize: (deltaPx: number) => {
                this.state.verticalHeight = Math.max(
                  100,
                  this.state.verticalHeight + deltaPx,
                );
                m.redraw();
              },
            }),
            m(
              '.pf-resize-demo-fixed',
              {
                style: {
                  height: '100px',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  background: 'var(--pf-color-background)',
                },
              },
              'Fixed panel below',
            ),
          ],
        ),
        m(CodeSnippet, {
          text: `m(ResizeHandle, {
  onResize: (deltaPx: number) => {
    this.height = Math.max(100, this.height + deltaPx);
    m.redraw();
  },
})`,
          language: 'typescript',
        }),
      ]),

      m('.pf-widget-doc-section', [
        m('h2', 'Horizontal ResizeHandle'),
        m(
          'p',
          'Set direction="horizontal" to resize horizontally. Drag the handle to resize the panel:',
        ),
        m(
          '.pf-resize-demo-container',
          {
            style: {
              border: '1px solid var(--pf-color-border)',
              borderRadius: '4px',
              overflow: 'hidden',
              display: 'flex',
              height: '200px',
              marginBottom: '16px',
            },
          },
          [
            m(
              '.pf-resize-demo-content',
              {
                style: {
                  width: `${this.state.horizontalWidth}px`,
                  background: 'var(--pf-color-background-secondary)',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  padding: '16px',
                },
              },
              `Width: ${Math.round(this.state.horizontalWidth)}px (drag handle to the right)`,
            ),
            m(ResizeHandle, {
              direction: 'horizontal',
              onResize: (deltaPx: number) => {
                this.state.horizontalWidth = Math.max(
                  150,
                  this.state.horizontalWidth + deltaPx,
                );
                m.redraw();
              },
            }),
            m(
              '.pf-resize-demo-fixed',
              {
                style: {
                  flex: 1,
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  background: 'var(--pf-color-background)',
                },
              },
              'Fixed panel to the right',
            ),
          ],
        ),
        m(CodeSnippet, {
          text: `m(ResizeHandle, {
  direction: 'horizontal',
  onResize: (deltaPx: number) => {
    this.width = Math.max(150, this.width + deltaPx);
    m.redraw();
  },
})`,
          language: 'typescript',
        }),
      ]),
    ];
  }
}

export function renderResizeHandle(): m.Children {
  return m(ResizeHandleDemo);
}
