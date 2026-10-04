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

import type {PropLink, PropRow} from './ui_hierarchy_props';
import {
  changeFlagNames,
  lastTransitionOf,
  type Transition,
  transitionEnd,
  transitionFlagNames,
  transitionLabel,
  transitionSpans,
  transitionModeName,
  transitionRows,
  transitionsAt,
  transitionStart,
  transitionTypeName,
} from './ui_hierarchy_transitions';

function text(rows: ReadonlyArray<PropRow>, label: string): string | undefined {
  const v = rows.find((r) => r.label === label)?.value;
  if (v === undefined) return undefined;
  if (v.kind === 'text') return v.text;
  return undefined;
}

function title(
  rows: ReadonlyArray<PropRow>,
  label: string,
): string | undefined {
  const v = rows.find((r) => r.label === label)?.value;
  if (v === undefined) return undefined;
  if (v.kind === 'text') return v.title;
  return undefined;
}

function links(
  rows: ReadonlyArray<PropRow>,
  label: string,
): ReadonlyArray<PropLink> | undefined {
  const v = rows.find((r) => r.label === label)?.value;
  if (v === undefined || v.kind !== 'links') return undefined;
  return v.links;
}

describe('transitionTypeName', () => {
  test('standard types', () => {
    expect(transitionTypeName(0)).toBe('NONE');
    expect(transitionTypeName(1)).toBe('OPEN');
    expect(transitionTypeName(2)).toBe('CLOSE');
    expect(transitionTypeName(3)).toBe('TO_FRONT');
    expect(transitionTypeName(4)).toBe('TO_BACK');
    expect(transitionTypeName(5)).toBe('RELAUNCH');
    expect(transitionTypeName(6)).toBe('CHANGE');
    expect(transitionTypeName(7)).toBe('KEYGUARD_GOING_AWAY');
    expect(transitionTypeName(8)).toBe('KEYGUARD_OCCLUDE');
    expect(transitionTypeName(9)).toBe('KEYGUARD_UNOCCLUDE');
    expect(transitionTypeName(10)).toBe('PIP');
    expect(transitionTypeName(11)).toBe('WAKE');
    expect(transitionTypeName(12)).toBe('SLEEP');
    expect(transitionTypeName(13)).toBe('PREPARE_BACK_NAVIGATION');
    expect(transitionTypeName(14)).toBe('CLOSE_PREPARE_BACK_NAVIGATION');
  });

  test('shell custom types', () => {
    expect(transitionTypeName(1001)).toBe('EXIT_PIP');
    expect(transitionTypeName(1002)).toBe('EXIT_PIP_TO_SPLIT');
    expect(transitionTypeName(1003)).toBe('REMOVE_PIP');
    expect(transitionTypeName(1004)).toBe('SPLIT_SCREEN_PAIR_OPEN');
    expect(transitionTypeName(1005)).toBe('SPLIT_SCREEN_OPEN_TO_SIDE');
    expect(transitionTypeName(1006)).toBe('SPLIT_DISMISS_SNAP');
    expect(transitionTypeName(1007)).toBe('SPLIT_DISMISS');
    expect(transitionTypeName(1008)).toBe('MAXIMIZE');
    expect(transitionTypeName(1009)).toBe('RESTORE_FROM_MAXIMIZE');
    expect(transitionTypeName(1010)).toBe('ENTER_FREEFORM');
    expect(transitionTypeName(1011)).toBe('ENTER_DESKTOP_MODE');
    expect(transitionTypeName(1012)).toBe('EXIT_DESKTOP_MODE');
    expect(transitionTypeName(1021)).toBe('CUSTOM+21');
  });

  test('unknown numeric types', () => {
    expect(transitionTypeName(99)).toBe('99');
  });
});

describe('transitionModeName', () => {
  test('standard modes', () => {
    expect(transitionModeName(0)).toBe('NONE');
    expect(transitionModeName(1)).toBe('OPEN');
    expect(transitionModeName(2)).toBe('CLOSE');
    expect(transitionModeName(3)).toBe('TO_FRONT');
    expect(transitionModeName(4)).toBe('TO_BACK');
    expect(transitionModeName(6)).toBe('CHANGE');
  });

  test('unrecognized modes formatted as number string', () => {
    expect(transitionModeName(5)).toBe('5');
    expect(transitionModeName(42)).toBe('42');
  });
});

describe('transitionFlagNames', () => {
  test('known flags', () => {
    expect(transitionFlagNames(128)).toEqual(['IS_RECENTS']);
    expect(transitionFlagNames(129)).toEqual([
      'KEYGUARD_GOING_AWAY_TO_SHADE',
      'IS_RECENTS',
    ]);
  });

  test('unknown bits and bit 31', () => {
    expect(transitionFlagNames(1 << 20)).toEqual(['0x100000']);
    expect(transitionFlagNames(1 << 31)).toEqual(['0x80000000']);
  });
});

describe('changeFlagNames', () => {
  test('known and unknown bits for sample values', () => {
    // 34603008: 0x2100000 = MOVED_TO_TOP | (1 << 25), a Shell custom bit.
    expect(changeFlagNames(34603008)).toEqual(['MOVED_TO_TOP', '0x2000000']);
    // 33554433: 0x2000001 = 1 | (1 << 25)
    expect(changeFlagNames(33554433)).toEqual(['SHOW_WALLPAPER', '0x2000000']);
  });

  test('bit 31 handling', () => {
    expect(changeFlagNames(1 << 31)).toEqual(['0x80000000']);
  });
});

describe('transitionStart and transitionEnd', () => {
  test('start prefers dispatchTs over sendTs', () => {
    expect(
      transitionStart({
        id: 1,
        type: 1,
        status: 'played',
        flags: 0,
        sendTs: 100n,
        dispatchTs: 200n,
        participants: [],
      }),
    ).toBe(200n);

    expect(
      transitionStart({
        id: 1,
        type: 1,
        status: 'played',
        flags: 0,
        sendTs: 100n,
        participants: [],
      }),
    ).toBe(100n);
  });

  test('merged transition end = mergeTs', () => {
    const merged: Transition = {
      id: 246,
      type: 1022,
      status: 'merged',
      flags: 0,
      mergeTs: 5000n,
      participants: [],
    };
    expect(transitionEnd(merged)).toBe(5000n);
  });

  test('end prefers finishTs over abortTs and mergeTs', () => {
    expect(
      transitionEnd({
        id: 1,
        type: 1,
        status: 'played',
        flags: 0,
        finishTs: 300n,
        abortTs: 250n,
        mergeTs: 200n,
        participants: [],
      }),
    ).toBe(300n);

    expect(
      transitionEnd({
        id: 1,
        type: 1,
        status: 'aborted',
        flags: 0,
        abortTs: 250n,
        mergeTs: 200n,
        participants: [],
      }),
    ).toBe(250n);
  });
});

describe('transitionsAt', () => {
  const t1: Transition = {
    id: 1,
    type: 1,
    status: 'played',
    flags: 0,
    dispatchTs: 100n,
    finishTs: 200n,
    participants: [],
  };

  const tOpenEnded: Transition = {
    id: 2,
    type: 1,
    status: 'playing',
    flags: 0,
    dispatchTs: 150n,
    participants: [],
  };

  test('boundaries', () => {
    expect(transitionsAt([t1], 99n)).toEqual([]);
    expect(transitionsAt([t1], 100n)).toEqual([t1]);
    expect(transitionsAt([t1], 150n)).toEqual([t1]);
    expect(transitionsAt([t1], 200n)).toEqual([t1]);
    expect(transitionsAt([t1], 201n)).toEqual([]);
  });

  test('open-ended transition matches any ts >= start', () => {
    expect(transitionsAt([tOpenEnded], 149n)).toEqual([]);
    expect(transitionsAt([tOpenEnded], 150n)).toEqual([tOpenEnded]);
    expect(transitionsAt([tOpenEnded], 1000n)).toEqual([tOpenEnded]);
  });
});

describe('lastTransitionOf', () => {
  const tEarlier: Transition = {
    id: 1,
    type: 1,
    status: 'played',
    flags: 0,
    dispatchTs: 100n,
    finishTs: 200n,
    participants: [{layerId: 10, mode: 1, flags: 0}],
  };
  const tLater: Transition = {
    id: 2,
    type: 2,
    status: 'played',
    flags: 0,
    dispatchTs: 300n,
    finishTs: 400n,
    participants: [{layerId: 10, mode: 2, flags: 0}],
  };
  const tOtherLayer: Transition = {
    id: 3,
    type: 1,
    status: 'played',
    flags: 0,
    dispatchTs: 350n,
    finishTs: 450n,
    participants: [{layerId: 99, mode: 1, flags: 0}],
  };

  test('picks latest-start <= ts and ignores later ones and non-participants', () => {
    const list = [tEarlier, tLater, tOtherLayer];
    const layerIds = new Set([10]);

    // Before both
    expect(lastTransitionOf(list, layerIds, 50n)).toBeUndefined();

    // Between earlier and later
    const mid = lastTransitionOf(list, layerIds, 250n);
    expect(mid?.transition.id).toBe(1);
    expect(mid?.participant.mode).toBe(1);

    // After later
    const after = lastTransitionOf(list, layerIds, 500n);
    expect(after?.transition.id).toBe(2);
    expect(after?.participant.mode).toBe(2);

    // Non-participating layer
    expect(lastTransitionOf(list, new Set([55]), 500n)).toBeUndefined();
  });
});

describe('transitionSpans', () => {
  const t = (id: number, start: bigint, end?: bigint): Transition => ({
    id,
    type: 1,
    status: 'played',
    flags: 0,
    dispatchTs: start,
    finishTs: end,
    participants: [],
  });
  const snaps = [10n, 20n, 30n, 40n].map((ts) => ({ts}));

  test('covers the snapshots taken while each one ran', () => {
    const spans = transitionSpans(
      [t(1, 15n, 30n), t(2, 21n, 29n), t(3, 40n, 90n), t(4, 50n)],
      snaps,
    );
    // 2 ran between snapshots; 4 never ended.
    expect(spans.map((s) => [s.transition.id, s.first, s.last])).toEqual([
      [1, 1, 2],
      [3, 3, 3],
    ]);
  });

  test('overlapping transitions get separate lanes', () => {
    const spans = transitionSpans(
      [t(1, 10n, 30n), t(2, 20n, 30n), t(3, 40n, 40n)],
      snaps,
    );
    expect(spans.map((s) => [s.transition.id, s.lane])).toEqual([
      [1, 0],
      [2, 1],
      [3, 0],
    ]);
  });

  test('label', () => {
    expect(transitionLabel(t(1, 0n, 527_000_000n))).toBe(
      'OPEN · played · 527ms',
    );
    expect(transitionLabel({...t(1, 0n), status: 'aborted'})).toBe(
      'OPEN · aborted',
    );
  });
});

describe('transitionRows', () => {
  test('played transition with participant and playing status when ts inside', () => {
    const t: Transition = {
      id: 242,
      type: 1,
      status: 'played',
      flags: 128,
      sendTs: 1_000_000_000n,
      dispatchTs: 2_000_000_000n,
      finishTs: 4_000_000_000n,
      handler: 'DefaultTransitionHandler',
      participants: [
        {layerId: 1409, mode: 1, flags: 34603008},
        {layerId: 45, mode: 4, flags: 33554433},
      ],
    };
    const participant = t.participants[0];
    const ts = 3_000_000_000n; // inside start..end

    const layerNames = new Map<number, string>([[1409, 'Task#1409']]);
    const rows = transitionRows(t, participant, ts, (id) => layerNames.get(id));

    // Verify exact labels in order
    expect(rows.map((r) => r.label)).toEqual([
      'Type',
      'Mode',
      'Status',
      'Started',
      'Duration',
      'Handler',
      'Flags',
      'Change flags',
      'Participants',
      'Id',
    ]);

    expect(text(rows, 'Type')).toBe('OPEN');
    expect(title(rows, 'Type')).toBe('1');
    expect(text(rows, 'Mode')).toBe('OPEN');
    expect(text(rows, 'Status')).toBe('playing');
    expect(text(rows, 'Started')).toBe('1s ago');
    expect(text(rows, 'Duration')).toBe('2s');
    expect(text(rows, 'Handler')).toBe('DefaultTransitionHandler');
    expect(text(rows, 'Flags')).toBe('IS_RECENTS');
    expect(title(rows, 'Flags')).toBe('0x80');
    expect(text(rows, 'Change flags')).toBe('MOVED_TO_TOP | 0x2000000');
    expect(title(rows, 'Change flags')).toBe('0x2100000');

    const participantLinks = links(rows, 'Participants');
    expect(participantLinks).toHaveLength(2);
    expect(participantLinks?.[0]).toEqual({
      text: 'Task#1409 · OPEN',
      target: {kind: 'layer', layerId: 1409},
    });
    expect(participantLinks?.[1]).toEqual({
      text: '#45 · TO_BACK',
      target: {kind: 'layer', layerId: 45},
    });

    const idRow = rows.find((r) => r.label === 'Id');
    expect(idRow?.value).toEqual({kind: 'text', text: '242', mono: true});
  });

  test('status not playing when outside', () => {
    const t: Transition = {
      id: 242,
      type: 1,
      status: 'played',
      flags: 0,
      dispatchTs: 2_000_000_000n,
      finishTs: 4_000_000_000n,
      participants: [],
    };
    const rows = transitionRows(t, undefined, 5_000_000_000n, () => undefined);
    expect(text(rows, 'Status')).toBe('played');
    expect(text(rows, 'Started')).toBe('3s ago');
  });
});
