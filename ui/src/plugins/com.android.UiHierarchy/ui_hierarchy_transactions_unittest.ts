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

import type {PropRow} from './ui_hierarchy_props';
import {
  appliedWith,
  changeName,
  changeSummary,
  groupLayerTransactions,
  type LayerChange,
  layerChangeRows,
  layerChangeSummary,
  type LayerTransaction,
  layerTransactionRows,
  senderLabel,
  senderRows,
  type TransactionDetails,
  type TransactionLog,
  transactionsQuery,
  transitionRoles,
} from './ui_hierarchy_transactions';
import type {Transition} from './ui_hierarchy_transitions';

const MS = 1_000_000n;

// A systemui buffer update, as SurfaceFlinger logs it.
const BUFFER_FLAGS = [
  'eBufferChanged',
  'eAcquireFenceChanged',
  'eDataspaceChanged',
  'eHdrMetadataChanged',
  'eSurfaceDamageRegionChanged',
  'eHasListenerCallbacksChanged',
  'eBufferCropChanged',
  'eDestinationFrameChanged',
];

function change(
  ts: bigint,
  layerId: number,
  transactionId: bigint,
  pid: number | undefined,
  changes: string[],
): LayerTransaction {
  return {ts, layerId, kind: 'changed', transactionId, pid, changes};
}

// 2899 systemui, 1394 system_server. Layers: 81 VRI-StatusBar (buffer),
// 80 StatusBar container, 271 its insets leash, 45 the wallpaper.
const ENTRIES: LayerTransaction[] = [
  change(1000n * MS, 81, 0xb53_0001n, 2899, BUFFER_FLAGS),
  change(1016n * MS, 81, 0xb53_0002n, 2899, BUFFER_FLAGS),
  // One shell transaction moving the leash and the wallpaper together.
  change(1020n * MS, 271, 0x572_0010n, 1394, [
    'ePositionChanged',
    'eAlphaChanged',
  ]),
  change(1020n * MS, 45, 0x572_0010n, 1394, ['ePositionChanged']),
  change(1033n * MS, 81, 0xb53_0003n, 2899, BUFFER_FLAGS),
  // The transition's start transaction, on the container.
  change(1040n * MS, 80, 0x572_0020n, 1394, ['eLayerChanged', 'eCropChanged']),
  // A transaction touching both the container and the buffer counts once.
  change(1050n * MS, 80, 0x572_0030n, 1394, ['eFlagsChanged']),
  change(1050n * MS, 81, 0x572_0030n, 1394, ['eFlagsChanged']),
  {
    ts: 1060n * MS,
    layerId: 999,
    kind: 'added',
    changes: [],
  },
  {
    ts: 1061n * MS,
    layerId: 999,
    kind: 'added',
    changes: [],
  },
  change(1062n * MS, 999, 0x9_0001n, undefined, ['eAlphaChanged']),
];

const NAMES = new Map([
  [2899, 'com.android.systemui'],
  [1394, 'system_server'],
]);

const LOG: TransactionLog = {
  after: 990n * MS,
  upTo: 1070n * MS,
  entries: ENTRIES,
  processNames: NAMES,
  truncated: false,
};

const OPEN: Transition = {
  id: 44,
  type: 1,
  status: 'played',
  flags: 0,
  startTransactionId: 0x572_0020n,
  finishTransactionId: 0x572_0029n,
  participants: [],
};

const LAYER_NAMES = new Map([
  [45, 'com.android.wallpaper.WallpaperService#45'],
  [80, 'ab1254f StatusBar#80'],
  [81, 'VRI-StatusBar#81'],
]);

const STATUS_BAR = new Set([80, 81, 271]);

function opts() {
  return {
    since: 'snapshot 14 · previous',
    transitions: [OPEN],
    layerName: (id: number) => LAYER_NAMES.get(id),
  };
}

function textRows(rows: ReadonlyArray<PropRow>): string[] {
  return rows.map((r) => {
    const v = r.value;
    switch (v.kind) {
      case 'text':
        return `${r.label}: ${v.text}`;
      case 'links':
        return `${r.label}: ${v.links.map((l) => l.text).join(', ')}`;
      default:
        return `${r.label}: <${v.kind}>`;
    }
  });
}

describe('changeName', () => {
  test('drops the e prefix and Changed suffix', () => {
    expect(changeName('ePositionChanged')).toBe('Position');
    expect(changeName('eBackgroundBlurRadiusChanged')).toBe(
      'Background blur radius',
    );
    expect(changeName('eFrameRateSelectionPriority')).toBe(
      'Frame rate selection priority',
    );
  });

  test('names z-order and parent changes', () => {
    expect(changeName('eLayerChanged')).toBe('Z');
    expect(changeName('eRelativeLayerChanged')).toBe('Relative Z');
    expect(changeName('eReparent')).toBe('Parent');
  });

  test('keeps unknown shapes', () => {
    expect(changeName('weird')).toBe('weird');
  });
});

describe('changeSummary', () => {
  test('folds the fields set with every buffer into Buffer', () => {
    expect(changeSummary(BUFFER_FLAGS)).toEqual(['Buffer']);
    expect(changeSummary([...BUFFER_FLAGS, 'eAlphaChanged'])).toEqual([
      'Buffer',
      'Alpha',
    ]);
  });

  test('keeps buffer-side fields without a new buffer', () => {
    expect(changeSummary(['eBufferCropChanged', 'eCropChanged'])).toEqual([
      'Buffer crop',
      'Crop',
    ]);
  });
});

describe('senderLabel', () => {
  test('shortens package names', () => {
    expect(senderLabel(2899, NAMES)).toBe('systemui');
    expect(senderLabel(1394, NAMES)).toBe('system_server');
  });

  test('falls back to the pid', () => {
    expect(senderLabel(42, NAMES)).toBe('pid 42');
    expect(senderLabel(undefined, NAMES)).toBe('Unknown sender');
  });
});

describe('groupLayerTransactions', () => {
  const groups = groupLayerTransactions(
    ENTRIES,
    STATUS_BAR,
    transitionRoles([OPEN]),
  );

  test('groups by sender, fields and role, latest first', () => {
    expect(
      groups.map((g) => [g.pid, g.summary.join(','), g.role, g.count]),
    ).toEqual([
      [1394, 'Flags', undefined, 1],
      [1394, 'Z,Crop', 'start of OPEN', 1],
      [2899, 'Buffer', undefined, 3],
      [1394, 'Position,Alpha', undefined, 1],
    ]);
  });

  test('keeps every changed field for the tooltip', () => {
    const buffer = groups.find((g) => g.pid === 2899);
    expect(buffer?.changes).toEqual(BUFFER_FLAGS);
    expect(buffer?.lastTs).toBe(1033n * MS);
  });

  test('ignores other layers and lifecycle events', () => {
    expect(
      groupLayerTransactions(ENTRIES, new Set([999]), new Map()),
    ).toHaveLength(1);
  });
});

describe('appliedWith', () => {
  test('lists the layers sharing transactions, excluding the own ones', () => {
    expect(appliedWith(ENTRIES, STATUS_BAR)).toEqual([{layerId: 45, count: 1}]);
  });

  test('a layer alone has none', () => {
    expect(appliedWith(ENTRIES, new Set([999]))).toEqual([]);
  });
});

describe('layerTransactionRows', () => {
  test('a window: senders, roles and the layers changed with it', () => {
    expect(textRows(layerTransactionRows(LOG, STATUS_BAR, opts()))).toEqual([
      'Since: snapshot 14 · previous',
      'system_server: Flags',
      'system_server: Z, Crop · start of OPEN',
      'systemui: Buffer · ×3',
      'system_server: Position, Alpha',
      'Applied with: WallpaperService#45',
    ]);
  });

  test('the tooltip names the process, the time and every field', () => {
    const rows = layerTransactionRows(LOG, new Set([81]), opts());
    const buffer = rows.find((r) => r.label === 'systemui')?.value;
    expect(buffer?.kind === 'text' && buffer.title).toBe(
      [
        'com.android.systemui (2899)',
        'Last 37ms before the snapshot',
        BUFFER_FLAGS.join(' | '),
      ].join('\n'),
    );
  });

  test('lifecycle events once each, and unknown senders', () => {
    expect(textRows(layerTransactionRows(LOG, new Set([999]), opts()))).toEqual(
      [
        'Since: snapshot 14 · previous',
        'Lifecycle: Created',
        'Unknown sender: Alpha',
      ],
    );
  });

  test('untouched layers say so', () => {
    expect(textRows(layerTransactionRows(LOG, new Set([7]), opts()))).toEqual([
      'Since: snapshot 14 · previous',
      'Changes: none',
    ]);
  });

  test('summarises beyond 8 groups and says when truncated', () => {
    const many = Array.from({length: 10}, (_, i) =>
      change(BigInt(1000 + i) * MS, 5, BigInt(i + 1), 2899, [`eF${i}Changed`]),
    );
    const rows = textRows(
      layerTransactionRows(
        {...LOG, entries: many, truncated: true},
        new Set([5]),
        opts(),
      ),
    );
    expect(rows).toHaveLength(1 + 8 + 2);
    expect(rows.at(-2)).toBe('More: 2 transactions in 2 groups');
    expect(rows.at(-1)).toBe('Truncated: first 20000 layer changes only');
  });
});

describe('senderRows', () => {
  test('busiest sender first, with distinct transactions and layers', () => {
    expect(textRows(senderRows(LOG, 'snapshot 14 · previous'))).toEqual([
      'Since: snapshot 14 · previous',
      'system_server: 3 transactions · 4 layers',
      'systemui: 3 transactions · 1 layer',
      'Unknown sender: 1 transaction · 1 layer',
    ]);
  });
});

function layerChange(
  layerId: number,
  changes: string[],
  values: Record<string, string>,
): LayerChange {
  return {layerId, changes, values: new Map(Object.entries(values))};
}

// What the shell set on the StatusBar leash, as SurfaceFlinger logs it.
const LEASH_MOVE = layerChange(271, ['ePositionChanged', 'eAlphaChanged'], {
  layer_id: '271',
  what: '3',
  x: '0',
  y: '1240.5',
  alpha: '0.600000023841858',
  apply_token: '12970367461511331488',
});

const name = (id: number) => LAYER_NAMES.get(id);

describe('layerChangeRows', () => {
  test('curates positions and numbers', () => {
    expect(textRows(layerChangeRows(LEASH_MOVE, name))).toEqual([
      'Position: (0, 1240.5)',
      'Alpha: 0.6',
    ]);
  });

  test('matrix, crop, corner radii, flags and buffers', () => {
    const c = layerChange(81, [], {
      'matrix.dsdx': '0.5',
      'matrix.dtdx': '0',
      'matrix.dtdy': '0',
      'matrix.dsdy': '0.5',
      'crop.left': '0',
      'crop.top': '0',
      'crop.right': '1080',
      'crop.bottom': '2400',
      'corner_radii.tl': '28',
      'corner_radii.tr': '28',
      'corner_radii.br': '28',
      'corner_radii.bl': '28',
      'flags': '1',
      'mask': '3',
      'buffer_data.width': '1080',
      'buffer_data.height': '2092',
      'buffer_data.pixel_format': 'PIXEL_FORMAT_RGBA_8888',
      'buffer_data.frame_number': '958',
      'buffer_crop.left': '0',
      'buffer_crop.top': '0',
      'buffer_crop.right': '0',
      'buffer_crop.bottom': '0',
      'transform': '0',
    });
    expect(textRows(layerChangeRows(c, name))).toEqual([
      'Matrix: [0.5, 0; 0, 0.5]',
      'Crop: [0, 0, 1080, 2400]',
      'Corner radius: 28 px',
      'Flags: HIDDEN on, OPAQUE off',
      'Buffer: 1080×2092 · RGBA_8888 · frame 958',
    ]);
  });

  test('parents link to layers, the root is none', () => {
    const rows = layerChangeRows(
      layerChange(80, ['eReparent'], {parent_id: '45'}),
      name,
    );
    expect(rows[0].value).toEqual({
      kind: 'links',
      links: [
        {
          text: 'WallpaperService#45',
          title: 'com.android.wallpaper.WallpaperService#45',
          target: {kind: 'layer', layerId: 45},
        },
      ],
    });
    expect(
      textRows(
        layerChangeRows(
          layerChange(80, ['eReparent'], {parent_id: '4294967295'}),
          name,
        ),
      ),
    ).toEqual(['Parent: none']);
  });

  test('other fields stay available raw, sorted', () => {
    const rows = layerChangeRows(
      layerChange(81, [], {
        'z': '3',
        'window_info_handle.focusable': 'true',
        'auto_refresh': 'false',
      }),
      name,
    );
    expect(textRows(rows)).toEqual(['Z: 3', 'Other fields: 2']);
    expect(textRows(rows[1].children ?? [])).toEqual([
      'auto_refresh: false',
      'window_info_handle.focusable: true',
    ]);
  });
});

describe('layerChangeSummary', () => {
  test('the first values, then an ellipsis', () => {
    expect(layerChangeSummary(LEASH_MOVE, name)).toBe(
      'Position (0, 1240.5) · Alpha 0.6',
    );
    const c = layerChange(81, [], {x: '1', y: '2', z: '3', alpha: '1'});
    expect(layerChangeSummary(c, name, 2)).toBe('Position (1, 2) · Z 3 · …');
  });

  test('the changed fields when nothing is curated', () => {
    expect(
      layerChangeSummary(layerChange(81, ['eInputInfoChanged'], {}), name),
    ).toBe('Input info');
  });
});

describe('expanded rows', () => {
  const details: TransactionDetails = {
    byId: new Map([
      [
        0x572_0010n,
        {
          id: 0x572_0010n,
          ts: 1020n * MS,
          vsyncId: 207369,
          pid: 1394,
          layers: [
            layerChange(45, ['ePositionChanged'], {x: '-30', y: '0'}),
            LEASH_MOVE,
          ],
        },
      ],
      [
        0x572_0020n,
        {
          id: 0x572_0020n,
          ts: 1040n * MS,
          pid: 1394,
          layers: [layerChange(80, ['eLayerChanged'], {z: '2'})],
        },
      ],
    ]),
  };
  const rows = layerTransactionRows(LOG, STATUS_BAR, {...opts(), details});

  test('groups list their transactions with the values they set', () => {
    const move = rows.find(
      (r) => r.value.kind === 'text' && r.value.text === 'Position, Alpha',
    );
    expect(textRows(move?.children ?? [])).toEqual([
      '−50ms: Position (0, 1240.5) · Alpha 0.6',
    ]);
  });

  test('a transaction shows its ids and every layer, own first', () => {
    const move = rows.find(
      (r) => r.value.kind === 'text' && r.value.text === 'Position, Alpha',
    );
    const tx = move?.children?.[0];
    expect(textRows(tx?.children ?? [])).toEqual([
      'Id: <copy>',
      'Sender: system_server (1394)',
      'Vsync: 207369',
      // The leash isn't in the snapshot's layers: no name.
      '#271: Position (0, 1240.5) · Alpha 0.6',
      'WallpaperService#45: Position (-30, 0)',
    ]);
    const wallpaper = tx?.children?.at(-1);
    expect(textRows(wallpaper?.children ?? [])).toEqual([
      'Layer: WallpaperService#45',
      'Position: (-30, 0)',
    ]);
  });

  test('start and finish transactions link to their transition', () => {
    const start = rows.find(
      (r) => r.value.kind === 'text' && r.value.text.endsWith('start of OPEN'),
    );
    const tx = start?.children?.[0];
    const link = tx?.children?.find((r) => r.label === 'Transition');
    expect(link?.value).toEqual({
      kind: 'links',
      links: [
        {
          text: 'start of OPEN',
          title: 'Compare across this transition',
          target: {kind: 'transition', transitionId: 44},
        },
      ],
    });
  });

  test('transactions not loaded are counted', () => {
    const buffer = rows.find((r) => r.label === 'systemui');
    expect(textRows(buffer?.children ?? [])).toEqual([
      'Older: 3 transactions: see Query',
    ]);
  });

  test('without details nothing expands', () => {
    expect(
      layerTransactionRows(LOG, STATUS_BAR, opts()).some(
        (r) => r.children !== undefined,
      ),
    ).toBe(false);
  });
});

describe('transactionsQuery', () => {
  test('scopes to the layers and the range', () => {
    const sql = transactionsQuery(new Set([80, 81]), 990n, 1070n);
    expect(sql).toContain('t.layer_id IN (80, 81)');
    expect(sql).toContain('s.ts > 990 AND s.ts <= 1070');
  });
});
