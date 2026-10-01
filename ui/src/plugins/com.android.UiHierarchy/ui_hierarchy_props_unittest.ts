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

import type {UiHierarchyNode} from './ui_hierarchy_data';
import {
  displayName,
  formatBounds,
  formatNumber,
  PropRowsBuilder,
  uiNodeRows,
} from './ui_hierarchy_props';

describe('PropRowsBuilder', () => {
  test('drops rows without a value', () => {
    const rows = new PropRowsBuilder()
      .text('Text', 'hello', {title: 'full'})
      .text('Empty', '')
      .text('Missing', undefined)
      .copy('Token', '0x1')
      .copy('No token', undefined)
      .links('No links', [])
      .links('Layer', [{text: 'A#1', target: {kind: 'layer', layerId: 1}}])
      .list('No items', [])
      .list('Reasons', ['a', 'b'])
      .chip('No chip', undefined)
      .chip('Composition', 'HWC')
      .build();
    expect(rows).toEqual([
      {label: 'Text', value: {kind: 'text', text: 'hello', title: 'full'}},
      {label: 'Token', value: {kind: 'copy', text: '0x1'}},
      {
        label: 'Layer',
        value: {
          kind: 'links',
          links: [{text: 'A#1', target: {kind: 'layer', layerId: 1}}],
        },
      },
      {label: 'Reasons', value: {kind: 'list', items: ['a', 'b']}},
      {label: 'Composition', value: {kind: 'chip', text: 'HWC'}},
    ]);
  });
});

describe('formatting', () => {
  test('numbers', () => {
    expect(formatNumber(28)).toBe('28');
    expect(formatNumber(0.4)).toBe('0.4');
    expect(formatNumber(1.254)).toBe('1.25');
    expect(formatNumber(2.5000001)).toBe('2.5');
    expect(formatNumber(-3)).toBe('-3');
  });

  test('bounds', () => {
    expect(formatBounds({left: 0, top: 0, right: 1080, bottom: 133})).toBe(
      '[0, 0, 1080, 133]  1080 x 133 px',
    );
    expect(formatBounds({left: 0.5, top: 0, right: 10, bottom: 2.25})).toBe(
      '[0.5, 0, 10, 2.25]  9.5 x 2.25 px',
    );
  });
});

describe('displayName', () => {
  test('activity titles drop the package', () => {
    expect(
      displayName(
        'com.google.android.apps.nexuslauncher/' +
          'com.google.android.apps.nexuslauncher.NexusLauncherActivity',
      ),
    ).toBe('NexusLauncherActivity');
    expect(displayName('com.example/.MainActivity')).toBe('MainActivity');
  });

  test('qualified class names drop the package', () => {
    expect(
      displayName(
        'com.google.pixel.wallpapers23.liveBirds.dynamic.DynamicCMFWallpaperService',
      ),
    ).toBe('DynamicCMFWallpaperService');
    expect(displayName('androidx.compose.ui.platform.ComposeView')).toBe(
      'ComposeView',
    );
    expect(displayName('android.widget.FrameLayout$Inner')).toBe(
      'FrameLayout$Inner',
    );
  });

  test('layer names drop the WM wrappers', () => {
    expect(
      displayName(
        'Surface(name=ab1254f StatusBar#80)/@0x31d9daa - animation-leash of insets_animation#271',
      ),
    ).toBe('insets_animation#271');
    expect(displayName('ab1254f StatusBar#80')).toBe('StatusBar#80');
    expect(
      displayName(
        'b8639ee com.google.pixel.wallpapers23.liveBirds.dynamic.DynamicCMFWallpaperService#39',
      ),
    ).toBe('DynamicCMFWallpaperService#39');
    expect(
      displayName(
        'f00ba12 com.google.android.apps.nexuslauncher/' +
          'com.google.android.apps.nexuslauncher.NexusLauncherActivity#120',
      ),
    ).toBe('NexusLauncherActivity#120');
  });

  test('other names are unchanged', () => {
    for (const name of [
      'VRI-StatusBar#81',
      'Task=1#35',
      'Offscreen Root',
      'StatusBar',
      'DisplayBackGestureHandler 0',
      'Splash Screen com.example',
      'com.android.systemui (2411)',
      'Column',
      '',
    ]) {
      expect(displayName(name)).toBe(name);
    }
  });
});

describe('uiNodeRows', () => {
  const node: UiHierarchyNode = {
    rowId: 1,
    ts: 0n,
    dur: 0n,
    windowId: 1,
    nodeId: '7',
    childIndex: 0,
    kind: 1,
    kindName: 'View',
    name: 'android.widget.Button',
    boundsLeft: 10,
    boundsTop: 20,
    boundsRight: 110,
    boundsBottom: 70,
    alpha: 1,
    flags: 0n,
    isVisible: true,
    isClickable: false,
    isFocused: false,
    isTextRedacted: false,
  };
  const labels = (n: UiHierarchyNode) => uiNodeRows(n).map((r) => r.label);

  test('defaults are left out', () => {
    expect(uiNodeRows(node)).toEqual([
      {
        label: 'Bounds',
        value: {
          kind: 'text',
          text: '[10, 20, 110, 70]  100 x 50 px',
          mono: true,
        },
      },
      {label: 'Visible', value: {kind: 'text', text: 'true'}},
    ]);
    expect(labels({...node, elevation: 0, recompositionCount: 0})).toEqual([
      'Bounds',
      'Visible',
    ]);
  });

  test('non-default values', () => {
    expect(
      labels({
        ...node,
        text: 'OK',
        testTag: 'ok_button',
        isClickable: true,
        isFocused: true,
        alpha: 0.5,
        elevation: 2,
        flags: 0x10n,
        recompositionCount: 3,
      }),
    ).toEqual([
      'Bounds',
      'Text',
      'Test tag',
      'Visible',
      'Clickable',
      'Focused',
      'Alpha',
      'Elevation',
      'Flags',
      'Recompositions',
    ]);
  });

  test('redacted text is not shown', () => {
    const rows = uiNodeRows({...node, text: 'hunter2', isTextRedacted: true});
    expect(JSON.stringify(rows)).not.toContain('hunter2');
    expect(rows[1]).toMatchObject({
      label: 'Text',
      value: {kind: 'text', text: 'redacted'},
    });
  });
});
