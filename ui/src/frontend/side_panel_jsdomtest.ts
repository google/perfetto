// Copyright (C) 2026 The Android Open Source Project
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
import {afterEach, beforeEach, describe, expect, test} from 'vitest';
import {SidePanelManagerImpl} from '../core/side_panel_manager';
import {SidePanelContainer} from './side_panel';

// UiMain, and the SidePanelContainer inside it, remounts whenever the trace or
// theme changes. The side panel's width must survive that.

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

// Renders the side panel. A new `key` remounts it, as UiMain's per-trace key
// does.
function renderSidePanel(sidePanelMgr: SidePanelManagerImpl, key: string) {
  m.render(
    container,
    m(SidePanelContainer, {key, sidePanelMgr, pageContent: 'page'}),
  );
}

function sidePanelWidth(): string | undefined {
  return container.querySelector<HTMLElement>('.pf-split-panel__second')?.style
    .flexBasis;
}

// Drags the resize handle so the side panel becomes `widthPx` wide. jsdom has
// no layout or pointer capture, so fake a 1000px wide container and stub the
// capture calls.
function dragSidePanelTo(widthPx: number) {
  const split = container.querySelector<HTMLElement>('.pf-split-panel')!;
  const handle = container.querySelector<HTMLElement>('.pf-resize-handle')!;
  split.getBoundingClientRect = () => ({width: 1000}) as DOMRect;
  handle.setPointerCapture = () => {};
  handle.releasePointerCapture = () => {};

  const clientX = 1000 - widthPx;
  for (const type of ['pointerdown', 'pointermove', 'pointerup']) {
    handle.dispatchEvent(new MouseEvent(type, {bubbles: true, clientX}));
  }
}

describe('SidePanelContainer', () => {
  test('keeps its width when remounted', () => {
    const sidePanelMgr = new SidePanelManagerImpl();
    sidePanelMgr.registerTab({uri: 'test#tab', title: 'Tab', render: () => ''});
    sidePanelMgr.showTab('test#tab');

    renderSidePanel(sidePanelMgr, 'trace-1');
    expect(sidePanelWidth()).toBe('400px');

    dragSidePanelTo(600);
    renderSidePanel(sidePanelMgr, 'trace-1');
    expect(sidePanelWidth()).toBe('600px');

    renderSidePanel(sidePanelMgr, 'trace-2');
    expect(sidePanelWidth()).toBe('600px');
  });
});
