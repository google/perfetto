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

import type {UiHierarchyNode, UiHierarchyWindow} from './ui_hierarchy_data';
import {
  buildProcessTreeNodes,
  displayRows,
  groupWindowsByProcess,
  ownerLabel,
  packageOfTitle,
  processRows,
  type ProcessRow,
  resolveWindowOwner,
  type WindowOwner,
  wmNodeRows,
} from './ui_hierarchy_screen';
import {
  buildWmNodes,
  SCREEN_KIND_PROCESS,
  WM_KIND_WINDOW,
} from './ui_hierarchy_wm';

const GSA = 'com.google.android.googlequicksearchbox';
const FLOATY =
  `${GSA}/com.google.android.apps.search.assistant.surfaces.voice.robin.` +
  'ui.floaty.activity.FloatyActivity';
const LAUNCHER =
  'com.google.android.apps.nexuslauncher/' +
  'com.google.android.apps.nexuslauncher.NexusLauncherActivity';

// `process` rows of the trace (package_list is empty).
const PROCESSES: ProcessRow[] = [
  {upid: 548, pid: 1461, name: 'system_server', uid: 1000},
  {upid: 5, pid: 2411, name: 'com.android.systemui', uid: 10275},
  {
    upid: 586,
    pid: 2415,
    name: 'com.google.android.apps.nexuslauncher',
    uid: 10274,
  },
  {upid: 569, pid: 2196, name: 'com.google.pixel.wallpapers23', uid: 10199},
  {upid: 4, pid: 4159, name: `${GSA}:search`, uid: 10177},
  {upid: 610, pid: 3208, name: `${GSA}:interactor`, uid: 10177},
  {upid: 648, pid: 4728, name: `${GSA}:googleapp`, uid: 10177},
  {upid: 1, pid: 2734, name: 'com.google.android.gms', uid: 10196},
  {upid: 663, pid: 5235, name: 'com.google.android.gms.ui', uid: 10196},
  {upid: 671, pid: 5572, name: 'com.google.android.gms.unstable', uid: 10196},
];

function uiWindow(title: string): UiHierarchyWindow {
  return {
    key: `5:${title}`,
    upid: 5,
    pid: 2411,
    processName: 'com.android.systemui',
    windowId: 1,
    title,
    displayId: 0,
    boundsLeft: 0,
    boundsTop: 0,
    boundsRight: 1080,
    boundsBottom: 2092,
  };
}

describe('resolveWindowOwner', () => {
  test('1: instrumented windows use the UI hierarchy process', () => {
    // ScreenDecorOverlayBottom has no buffer layer, but SystemUI records it.
    const o = resolveWindowOwner(
      {
        title: 'ScreenDecorOverlayBottom',
        uiWindow: uiWindow('ScreenDecorOverlayBottom'),
      },
      PROCESSES,
    );
    expect(o).toMatchObject({
      key: 'upid:5',
      name: 'com.android.systemui',
      pid: 2411,
      source: 'ui_hierarchy',
    });
  });

  test('2: buffer layer uid maps to the app process', () => {
    // VRI-Taskbar#70 is owned by uid 10274.
    expect(
      resolveWindowOwner({title: 'Taskbar', appUid: 10274}, PROCESSES),
    ).toMatchObject({
      key: 'upid:586',
      name: 'com.google.android.apps.nexuslauncher',
      pid: 2415,
      source: 'layer_uid',
    });
    // The same process as instrumented windows of that app.
    expect(
      resolveWindowOwner({title: 'StatusBar', appUid: 10275}, PROCESSES).key,
    ).toBe('upid:5');
  });

  test('2: several processes per uid prefer the main one', () => {
    expect(
      resolveWindowOwner({title: 'GmsOverlay', appUid: 10196}, PROCESSES).name,
    ).toBe('com.google.android.gms');
    // No main process: the package match, else the latest one.
    expect(
      resolveWindowOwner({title: FLOATY, appUid: 10177}, PROCESSES).name,
    ).toBe(`${GSA}:googleapp`);
  });

  test('2: unknown uid keeps the uid', () => {
    expect(
      resolveWindowOwner({title: 'Overlay', appUid: 10999}, PROCESSES),
    ).toEqual({
      key: 'uid:10999',
      name: 'uid 10999',
      uid: 10999,
      source: 'layer_uid',
    });
  });

  test('3: package in the title without a process row', () => {
    // FloatyActivity has no buffer layer; GSA only has ':suffix' processes.
    const o = resolveWindowOwner({title: FLOATY}, PROCESSES);
    expect(o).toEqual({key: `package:${GSA}`, name: GSA, source: 'package'});
    expect(ownerLabel(o)).toBe(GSA);
  });

  test('3: package in the title with a process row', () => {
    expect(resolveWindowOwner({title: LAUNCHER}, PROCESSES)).toMatchObject({
      key: 'upid:586',
      pid: 2415,
      source: 'package',
    });
  });

  test('4: system windows fall back to system_server', () => {
    // InputMethod: container owner_uid 1000, no buffer layer, no package.
    const o = resolveWindowOwner({title: 'InputMethod'}, PROCESSES);
    expect(o).toMatchObject({
      key: 'upid:548',
      name: 'system_server',
      pid: 1461,
      source: 'system',
    });
    expect(ownerLabel(o)).toBe('system_server (1461)');
    expect(
      resolveWindowOwner({title: 'InputMethod', appUid: 1000}, PROCESSES)
        .source,
    ).toBe('system');
    expect(resolveWindowOwner({title: 'InputMethod'}, [])).toEqual({
      key: 'system',
      name: 'system_server',
      uid: 1000,
      source: 'system',
    });
  });
});

describe('packageOfTitle', () => {
  test('activity titles', () => {
    expect(packageOfTitle(FLOATY)).toBe(GSA);
    expect(packageOfTitle('com.example/.Main')).toBe('com.example');
    expect(packageOfTitle('StatusBar')).toBeUndefined();
    expect(packageOfTitle('DisplayBackGestureHandler 0')).toBeUndefined();
  });
});

describe('process tree', () => {
  // Windows of one display, back to front.
  const windows = buildWmNodes(
    [
      [
        'wallpaper',
        'com.google.pixel.wallpapers23.liveBirds.dynamic.DynamicCMFWallpaperService',
      ],
      ['launcher', LAUNCHER],
      ['ime', 'InputMethod'],
      ['statusbar', 'StatusBar'],
      ['shade', 'NotificationShade'],
      ['taskbar', 'Taskbar'],
      ['decor', 'ScreenDecorOverlayBottom'],
    ].map(([, title], i) => ({
      token: i + 1,
      childIndex: i,
      title,
      containerType: 'WindowState',
      isVisible: title !== 'NotificationShade',
      args: new Map(),
    })),
    1n,
  );
  const owners: Record<string, WindowOwner> = {
    systemui: {
      key: 'upid:5',
      name: 'com.android.systemui',
      pid: 2411,
      upid: 5,
      source: 'ui_hierarchy',
    },
    launcher: {
      key: 'upid:586',
      name: 'com.google.android.apps.nexuslauncher',
      pid: 2415,
      upid: 586,
      source: 'layer_uid',
    },
    wallpaper: {
      key: 'upid:569',
      name: 'com.google.pixel.wallpapers23',
      pid: 2196,
      upid: 569,
      source: 'layer_uid',
    },
    system: {
      key: 'upid:548',
      name: 'system_server',
      pid: 1461,
      upid: 548,
      source: 'system',
    },
  };
  const ownerOf = (w: UiHierarchyNode): WindowOwner => {
    switch (w.name) {
      case 'StatusBar':
      case 'NotificationShade':
      case 'ScreenDecorOverlayBottom':
        return owners.systemui;
      case 'InputMethod':
        return owners.system;
      case 'Taskbar':
      case LAUNCHER:
        return owners.launcher;
      default:
        return owners.wallpaper;
    }
  };

  test('groups by topmost window, windows top first', () => {
    const groups = groupWindowsByProcess(windows, ownerOf);
    expect(groups.map((g) => g.owner.name)).toEqual([
      'com.android.systemui',
      'com.google.android.apps.nexuslauncher',
      'system_server',
      'com.google.pixel.wallpapers23',
    ]);
    expect(groups[0].windows.map((w) => w.name)).toEqual([
      'ScreenDecorOverlayBottom',
      'NotificationShade',
      'StatusBar',
    ]);
    expect(groups[1].windows.map((w) => w.name)).toEqual(['Taskbar', LAUNCHER]);
  });

  test('process nodes parent the window nodes', () => {
    const nodes = buildProcessTreeNodes(
      groupWindowsByProcess(windows, ownerOf),
      1n,
    );
    const proc = nodes[0];
    expect(proc).toMatchObject({
      nodeId: 'process:upid:5',
      kind: SCREEN_KIND_PROCESS,
      name: 'com.android.systemui (2411)',
    });
    expect(proc.parentNodeId).toBeUndefined();
    const children = nodes.filter((n) => n.parentNodeId === proc.nodeId);
    expect(children.map((n) => [n.name, n.kind, n.childIndex])).toEqual([
      ['ScreenDecorOverlayBottom', WM_KIND_WINDOW, 0],
      ['NotificationShade', WM_KIND_WINDOW, 1],
      ['StatusBar', WM_KIND_WINDOW, 2],
    ]);
    // Windows keep their WM node ids, so selection is shared with WM mode.
    expect(children[2].nodeId).toBe('4');
    // The source WM nodes are not modified.
    expect(windows[3].parentNodeId).toBeUndefined();
  });

  test('window properties link to the process', () => {
    const statusBar: UiHierarchyNode = {
      ...windows[3],
      wm: {
        containerType: 'WindowState',
        token: 4,
        windowType: 2000,
        layerId: 80,
      },
    };
    const rows = wmNodeRows(statusBar, owners.systemui);
    expect(rows.map((r) => r.label)).toEqual([
      'Frame',
      'Visible',
      'Type',
      'Layer id',
      'Token',
      'Process',
    ]);
    expect(rows[2].value).toEqual({
      kind: 'text',
      text: 'STATUS_BAR (2000)',
      mono: true,
    });
    expect(rows[5].value).toEqual({
      kind: 'links',
      links: [
        {
          text: 'com.android.systemui (2411)',
          title: 'UI hierarchy data recorded by the process',
          target: {kind: 'process', key: 'upid:5'},
        },
      ],
    });
    // Alpha only when not opaque.
    const dimmed = wmNodeRows({...statusBar, alpha: 0.5}, undefined);
    expect(dimmed.map((r) => r.label)).toContain('Alpha');
    expect(dimmed.map((r) => r.label)).not.toContain('Process');
  });

  test('process properties link to the windows', () => {
    const [systemui, , system] = groupWindowsByProcess(windows, ownerOf);
    const rows = processRows(systemui);
    // No uid known: no UID row.
    expect(rows.map((r) => r.label)).toEqual(['Attribution', 'Windows']);
    expect(rows[1].value).toMatchObject({
      kind: 'links',
      links: [
        {
          text: 'ScreenDecorOverlayBottom',
          target: {kind: 'window', nodeId: '7'},
        },
        {text: 'NotificationShade', target: {kind: 'window', nodeId: '5'}},
        {text: 'StatusBar', target: {kind: 'window', nodeId: '4'}},
      ],
    });
    const withUid = {...system, owner: {...system.owner, uid: 1000}};
    expect(processRows(withUid)[0]).toEqual({
      label: 'UID',
      value: {kind: 'text', text: '1000'},
    });
  });
});

describe('displayRows', () => {
  test('summary and layer counts', () => {
    const rows = displayRows(
      {
        width: 1080,
        height: 2092,
        dpi: 420,
        focusedApp:
          'com.google.android.apps.nexuslauncher/.NexusLauncherActivity',
        currentFocus: LAUNCHER,
      },
      {visible: 12, hwc: 10, gpu: 2},
    );
    expect(rows).toEqual([
      {label: 'Size', value: {kind: 'text', text: '1080 x 2092 px'}},
      {label: 'Density', value: {kind: 'text', text: '420 dpi'}},
      {
        label: 'Focused app',
        value: {
          kind: 'text',
          text: 'NexusLauncherActivity',
          title: 'com.google.android.apps.nexuslauncher/.NexusLauncherActivity',
        },
      },
      {
        label: 'Current focus',
        value: {kind: 'text', text: 'NexusLauncherActivity', title: LAUNCHER},
      },
      {
        label: 'Visible layers',
        value: {kind: 'text', text: '12 (10 HWC, 2 GPU)'},
      },
    ]);
  });

  test('without WM summary or SF data', () => {
    expect(displayRows(undefined, undefined)).toEqual([]);
    expect(displayRows({width: 1080}, undefined)).toEqual([]);
  });
});
