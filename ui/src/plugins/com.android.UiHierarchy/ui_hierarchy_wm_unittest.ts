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

import {kindToString, type UiHierarchyWindow} from './ui_hierarchy_data';
import {
  buildDisplaySummary,
  buildWmNodes,
  defaultDisplay,
  displayOf,
  matchUiWindow,
  normalizeWmArgKey,
  WM_KIND_DISPLAY,
  WM_KIND_WINDOW,
  windowTypeToString,
  type WmContainerRow,
} from './ui_hierarchy_wm';

function row(
  token: number,
  parentToken: number | undefined,
  childIndex: number,
  containerType: string,
  title: string,
  args: Record<string, number> = {},
  isVisible = true,
): WmContainerRow {
  return {
    token,
    parentToken,
    childIndex,
    containerType,
    title,
    isVisible,
    args: new Map(Object.entries(args)),
  };
}

function uiWindow(
  key: string,
  title: string,
  bounds = [0, 0, 1080, 2092],
): UiHierarchyWindow {
  return {
    key,
    upid: 1,
    windowId: 1,
    title,
    displayId: 0,
    boundsLeft: bounds[0],
    boundsTop: bounds[1],
    boundsRight: bounds[2],
    boundsBottom: bounds[3],
  };
}

// Root > Display > [DisplayArea(wallpaper) > Window, DisplayArea(top) >
// [StatusBar, NotificationShade]], children given out of order.
const ROWS: WmContainerRow[] = [
  row(1, undefined, 0, 'RootWindowContainer', 'WindowContainer'),
  row(2, 1, 0, 'DisplayContent', 'Outer Display', {
    'bounds.right': 1080,
    'bounds.bottom': 2092,
  }),
  row(12, 2, 1, 'DisplayArea', 'Leaf:36:36'),
  row(11, 2, 0, 'DisplayArea', 'Leaf:1:1'),
  row(21, 11, 0, 'WindowState', 'Wallpaper', {
    'frame.right': 1080,
    'frame.bottom': 2092,
    'bounds.right': 99,
  }),
  row(32, 12, 1, 'WindowState', 'NotificationShade', {
    'frame.right': 1080,
    'frame.bottom': 2092,
    'type': 2040,
  }),
  row(31, 12, 0, 'WindowState', 'StatusBar', {
    'frame.right': 1080,
    'frame.bottom': 133,
  }),
];

describe('UiHierarchyWm', () => {
  describe('normalizeWmArgKey', () => {
    it('maps frames, bounds and scalar args', () => {
      expect(normalizeWmArgKey('window.window_frames.frame.left')).toBe(
        'frame.left',
      );
      expect(
        normalizeWmArgKey(
          'task.task_fragment.window_container.configuration_container.' +
            'full_configuration.window_configuration.bounds.bottom',
        ),
      ).toBe('bounds.bottom');
      expect(normalizeWmArgKey('window.attributes.type')).toBe('type');
      expect(normalizeWmArgKey('window.attributes.alpha')).toBe('alpha');
      expect(
        normalizeWmArgKey('window.window_container.surface_control.layerId'),
      ).toBe('layerId');
    });

    it('ignores unrelated keys', () => {
      expect(
        normalizeWmArgKey('window.window_frames.display_frame.right'),
      ).toBeUndefined();
      expect(
        normalizeWmArgKey(
          'window.window_container.configuration_container.' +
            'merged_override_configuration.window_configuration.bounds.right',
        ),
      ).toBeUndefined();
      expect(normalizeWmArgKey('window.requested_width')).toBeUndefined();
    });
  });

  describe('buildWmNodes', () => {
    const nodes = buildWmNodes(ROWS, 5n);

    it('orders nodes depth-first, children by child_index (z order)', () => {
      expect(nodes.map((n) => n.name)).toEqual([
        'WindowContainer',
        'Outer Display',
        'Leaf:1:1',
        'Wallpaper',
        'Leaf:36:36',
        'StatusBar',
        'NotificationShade',
      ]);
    });

    it('prefers the window frame over configuration bounds', () => {
      const wallpaper = nodes.find((n) => n.name === 'Wallpaper');
      expect(wallpaper?.boundsRight).toBe(1080);
      expect(wallpaper?.boundsBottom).toBe(2092);
    });

    it('defaults omitted (zero) sides to 0', () => {
      const bar = nodes.find((n) => n.name === 'StatusBar');
      expect([
        bar?.boundsLeft,
        bar?.boundsTop,
        bar?.boundsRight,
        bar?.boundsBottom,
      ]).toEqual([0, 0, 1080, 133]);
    });

    it('sets kinds, parents and WM info', () => {
      const shade = nodes.find((n) => n.name === 'NotificationShade');
      expect(shade?.kind).toBe(WM_KIND_WINDOW);
      expect(kindToString(shade?.kind ?? 0)).toBe('Window');
      expect(shade?.parentNodeId).toBe('12');
      expect(shade?.wm?.windowType).toBe(2040);
      expect(nodes[0].parentNodeId).toBeUndefined();
    });

    it('treats containers with a missing parent as roots', () => {
      const orphan = buildWmNodes([row(5, 404, 0, 'Task', 'Orphan')], 0n);
      expect(orphan[0].parentNodeId).toBeUndefined();
    });

    it('terminates on cycles', () => {
      const cyc = buildWmNodes(
        [row(1, 2, 0, 'Task', 'A'), row(2, 1, 0, 'Task', 'B')],
        0n,
      );
      expect(cyc.length).toBeLessThanOrEqual(2);
    });
  });

  describe('displays', () => {
    const nodes = buildWmNodes(
      [
        ...ROWS,
        // An empty virtual display listed first.
        row(3, 1, -1, 'DisplayContent', 'VideoEncode-0'),
      ],
      0n,
    );
    const byNodeId = new Map(nodes.map((n) => [n.nodeId, n]));

    it('finds the display of a node', () => {
      expect(displayOf(byNodeId, '31')?.name).toBe('Outer Display');
      expect(displayOf(byNodeId, '1')).toBeUndefined();
    });

    it('defaults to the display with the most visible windows', () => {
      const d = defaultDisplay(nodes);
      expect(d?.kind).toBe(WM_KIND_DISPLAY);
      expect(d?.name).toBe('Outer Display');
    });
  });

  describe('matchUiWindow', () => {
    const nodes = buildWmNodes(ROWS, 0n);
    const shade = nodes.find((n) => n.name === 'NotificationShade');
    const bar = nodes.find((n) => n.name === 'StatusBar');
    const display = nodes.find((n) => n.name === 'Outer Display');

    it('matches by title', () => {
      const w = [
        uiWindow('a', 'StatusBar'),
        uiWindow('b', 'NotificationShade'),
      ];
      expect(shade && matchUiWindow(shade, w)?.key).toBe('b');
    });

    it('returns undefined without a match or for non-windows', () => {
      expect(shade && matchUiWindow(shade, [uiWindow('a', 'X')])).toBe(
        undefined,
      );
      expect(
        display && matchUiWindow(display, [uiWindow('a', 'Outer Display')]),
      ).toBe(undefined);
    });

    it('resolves duplicate titles by bounds', () => {
      const w = [
        uiWindow('full', 'StatusBar', [0, 0, 1080, 2092]),
        uiWindow('strip', 'StatusBar', [0, 0, 1080, 133]),
      ];
      expect(bar && matchUiWindow(bar, w)?.key).toBe('strip');
    });

    it('matches only windows on the same display', () => {
      const other = {...uiWindow('d2', 'StatusBar'), displayId: 2};
      const main = uiWindow('d0', 'StatusBar', [0, 0, 1, 1]);
      expect(bar && matchUiWindow(bar, [other, main], 0)?.key).toBe('d0');
      expect(bar && matchUiWindow(bar, [other], 0)).toBe(undefined);
      expect(bar && matchUiWindow(bar, [other], 2)?.key).toBe('d2');
    });

    it('defaults the display id to 0', () => {
      expect(display?.wm?.displayId).toBe(0);
      const d10 = buildWmNodes(
        [row(1, undefined, 0, 'DisplayContent', 'V', {displayId: 10})],
        0n,
      );
      expect(d10[0].wm?.displayId).toBe(10);
      expect(normalizeWmArgKey('display_content.id')).toBe('displayId');
    });
  });

  it('names window types', () => {
    expect(windowTypeToString(2040)).toBe('NOTIFICATION_SHADE (2040)');
    expect(windowTypeToString(4242)).toBe('4242');
  });

  it('summarizes displays', () => {
    const s = buildDisplaySummary(
      new Map([
        ['display_content.display_info.logical_width', '1080'],
        ['display_content.display_info.logical_height', '2092'],
        ['display_content.dpi', '420'],
        [
          'display_content.focused_app',
          'com.google.android.apps.nexuslauncher/.NexusLauncherActivity',
        ],
        [
          'display_content.current_focus_identifier.title',
          'com.google.android.apps.nexuslauncher/' +
            'com.google.android.apps.nexuslauncher.NexusLauncherActivity',
        ],
      ]),
    );
    expect(s).toEqual({
      width: 1080,
      height: 2092,
      dpi: 420,
      focusedApp:
        'com.google.android.apps.nexuslauncher/.NexusLauncherActivity',
      resumedActivity: undefined,
      currentFocus:
        'com.google.android.apps.nexuslauncher/' +
        'com.google.android.apps.nexuslauncher.NexusLauncherActivity',
    });
  });
});
