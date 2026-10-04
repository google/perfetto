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

// WindowManager Shell transitions (com.android.wm.shell.transition): which
// layers each transition moved, how, and when, for the Screen level.

import {Duration} from '../../base/time';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {type PropRow, PropRowsBuilder} from './ui_hierarchy_props';

// A layer a transition moved, usually a task or the wallpaper token.
export interface TransitionParticipant {
  readonly layerId: number;
  readonly mode: number;
  readonly flags: number;
}

export interface Transition {
  readonly id: number; // transition_id
  readonly type: number;
  readonly status: string;
  readonly handler?: string; // simple class name, e.g. 'DefaultTransitionHandler'
  readonly flags: number; // 0 when NULL
  readonly sendTs?: bigint;
  readonly dispatchTs?: bigint;
  readonly finishTs?: bigint;
  readonly abortTs?: bigint; // shell_abort ?? wm_abort
  readonly mergeTs?: bigint;
  // The SurfaceFlinger transactions that start and finish the animation.
  readonly startTransactionId?: bigint;
  readonly finishTransactionId?: bigint;
  readonly participants: ReadonlyArray<TransitionParticipant>;
}

const TRANSITION_TYPE_NAMES: ReadonlyMap<number, string> = new Map([
  [0, 'NONE'],
  [1, 'OPEN'],
  [2, 'CLOSE'],
  [3, 'TO_FRONT'],
  [4, 'TO_BACK'],
  [5, 'RELAUNCH'],
  [6, 'CHANGE'],
  [7, 'KEYGUARD_GOING_AWAY'],
  [8, 'KEYGUARD_OCCLUDE'],
  [9, 'KEYGUARD_UNOCCLUDE'],
  [10, 'PIP'],
  [11, 'WAKE'],
  [12, 'SLEEP'],
  [13, 'PREPARE_BACK_NAVIGATION'],
  [14, 'CLOSE_PREPARE_BACK_NAVIGATION'],
  [1001, 'EXIT_PIP'],
  [1002, 'EXIT_PIP_TO_SPLIT'],
  [1003, 'REMOVE_PIP'],
  [1004, 'SPLIT_SCREEN_PAIR_OPEN'],
  [1005, 'SPLIT_SCREEN_OPEN_TO_SIDE'],
  [1006, 'SPLIT_DISMISS_SNAP'],
  [1007, 'SPLIT_DISMISS'],
  [1008, 'MAXIMIZE'],
  [1009, 'RESTORE_FROM_MAXIMIZE'],
  [1010, 'ENTER_FREEFORM'],
  [1011, 'ENTER_DESKTOP_MODE'],
  [1012, 'EXIT_DESKTOP_MODE'],
]);

// WindowManager.TransitionType, with the Shell's custom types.
export function transitionTypeName(type: number): string {
  const name = TRANSITION_TYPE_NAMES.get(type);
  if (name !== undefined) return name;
  if (type >= 1000) return `CUSTOM+${type - 1000}`;
  return `${type}`;
}

const TRANSITION_MODE_NAMES: ReadonlyMap<number, string> = new Map([
  [0, 'NONE'],
  [1, 'OPEN'],
  [2, 'CLOSE'],
  [3, 'TO_FRONT'],
  [4, 'TO_BACK'],
  [6, 'CHANGE'],
]);

// TransitionInfo.TransitionMode of a participant.
export function transitionModeName(mode: number): string {
  return TRANSITION_MODE_NAMES.get(mode) ?? `${mode}`;
}

const TRANSITION_FLAGS: ReadonlyArray<readonly [number, string]> = [
  [1 << 0, 'KEYGUARD_GOING_AWAY_TO_SHADE'],
  [1 << 1, 'KEYGUARD_GOING_AWAY_NO_ANIMATION'],
  [1 << 2, 'KEYGUARD_GOING_AWAY_WITH_WALLPAPER'],
  [1 << 3, 'KEYGUARD_GOING_AWAY_SUBTLE_ANIMATION'],
  [1 << 4, 'APP_CRASHED'],
  [1 << 5, 'OPEN_BEHIND'],
  [1 << 6, 'KEYGUARD_LOCKED'],
  [1 << 7, 'IS_RECENTS'],
  [1 << 8, 'KEYGUARD_GOING_AWAY'],
  [1 << 9, 'KEYGUARD_GOING_AWAY_TO_LAUNCHER_CLEAR_SNAPSHOT'],
  [1 << 10, 'INVISIBLE'],
  [1 << 11, 'KEYGUARD_APPEARING'],
];

// Names of the bits of `flags` in `table`, then the unknown bits as hex.
function flagNames(
  flags: number,
  table: ReadonlyArray<readonly [number, string]>,
): string[] {
  const names: string[] = [];
  let rest = flags >>> 0;
  for (const [bit, name] of table) {
    if ((rest & bit) !== 0) {
      names.push(name);
      rest = (rest & ~bit) >>> 0;
    }
  }
  for (let i = 0; i < 32; i++) {
    const mask = (1 << i) >>> 0;
    if ((rest & mask) !== 0) names.push(`0x${mask.toString(16)}`);
  }
  return names;
}

// WindowManager.TransitionFlags.
export function transitionFlagNames(flags: number): string[] {
  return flagNames(flags, TRANSITION_FLAGS);
}

const CHANGE_FLAGS: ReadonlyArray<readonly [number, string]> = [
  [1 << 0, 'SHOW_WALLPAPER'],
  [1 << 1, 'IS_WALLPAPER'],
  [1 << 2, 'TRANSLUCENT'],
  [1 << 3, 'STARTING_WINDOW_TRANSFER_RECIPIENT'],
  [1 << 4, 'IS_VOICE_INTERACTION'],
  [1 << 5, 'IS_DISPLAY'],
  [1 << 7, 'DISPLAY_HAS_ALERT_WINDOWS'],
  [1 << 8, 'IS_INPUT_METHOD'],
  [1 << 9, 'IN_TASK_WITH_EMBEDDED_ACTIVITY'],
  [1 << 10, 'FILLS_TASK'],
  [1 << 11, 'WILL_IME_SHOWN'],
  [1 << 12, 'CROSS_PROFILE_OWNER_THUMBNAIL'],
  [1 << 13, 'CROSS_PROFILE_WORK_THUMBNAIL'],
  [1 << 14, 'IS_BEHIND_STARTING_WINDOW'],
  [1 << 15, 'IS_OCCLUDED'],
  [1 << 16, 'IS_SYSTEM_WINDOW'],
  [1 << 17, 'BACK_GESTURE_ANIMATED'],
  [1 << 18, 'NO_ANIMATION'],
  [1 << 19, 'TASK_LAUNCHING_BEHIND'],
  [1 << 20, 'MOVED_TO_TOP'],
  [1 << 21, 'SYNC'],
  [1 << 22, 'CONFIG_AT_END'],
  [1 << 23, 'IS_TASK_DISPLAY_AREA'],
  // Bits from 1 << 24 up are custom to the Shell.
];

// TransitionInfo.ChangeFlags of a participant.
export function changeFlagNames(flags: number): string[] {
  return flagNames(flags, CHANGE_FLAGS);
}

// When the transition started playing, or was sent if never dispatched.
export function transitionStart(t: Transition): bigint | undefined {
  return t.dispatchTs ?? t.sendTs;
}

// When it finished, was aborted, or was merged into another one.
export function transitionEnd(t: Transition): bigint | undefined {
  return t.finishTs ?? t.abortTs ?? t.mergeTs;
}

// The transitions running at `ts`.
export function transitionsAt(
  list: ReadonlyArray<Transition>,
  ts: bigint,
): Transition[] {
  return list.filter((t) => {
    const start = transitionStart(t);
    if (start === undefined || start > ts) return false;
    const end = transitionEnd(t);
    return end === undefined || ts <= end;
  });
}

// The latest transition started at or before `ts` that moved one of
// `layerIds`, with the participant for it.
export function lastTransitionOf(
  list: ReadonlyArray<Transition>,
  layerIds: ReadonlySet<number>,
  ts: bigint,
):
  | {
      readonly transition: Transition;
      readonly participant: TransitionParticipant;
    }
  | undefined {
  let best:
    | {
        transition: Transition;
        participant: TransitionParticipant;
        start: bigint;
      }
    | undefined = undefined;

  for (const t of list) {
    const start = transitionStart(t);
    if (start === undefined || start > ts) continue;
    const p = t.participants.find((part) => layerIds.has(part.layerId));
    if (p === undefined) continue;
    if (best === undefined || start > best.start) {
      best = {transition: t, participant: p, start};
    }
  }

  if (best === undefined) return undefined;
  return {transition: best.transition, participant: best.participant};
}

// Where a transition falls on a timeline of snapshots.
export interface TransitionSpan {
  readonly transition: Transition;
  // Indices of the first and last snapshot taken while it ran.
  readonly first: number;
  readonly last: number;
  // Row to draw it in, so overlapping transitions don't hide each other.
  readonly lane: number;
}

// The transitions that ran while at least one of `snapshots` (by time) was
// taken, with the snapshots they cover.
export function transitionSpans(
  list: ReadonlyArray<Transition>,
  snapshots: ReadonlyArray<{readonly ts: bigint}>,
): TransitionSpan[] {
  const spans: TransitionSpan[] = [];
  const laneEnds: number[] = [];
  for (const t of [...list].sort((a, b) =>
    Number((transitionStart(a) ?? 0n) - (transitionStart(b) ?? 0n)),
  )) {
    const start = transitionStart(t);
    const end = transitionEnd(t);
    if (start === undefined || end === undefined) continue;
    let first = -1;
    let last = -1;
    snapshots.forEach((s, i) => {
      if (s.ts < start || s.ts > end) return;
      if (first < 0) first = i;
      last = i;
    });
    if (first < 0) continue;
    // The first lane free since before `first`.
    let lane = laneEnds.findIndex((end) => end < first);
    if (lane < 0) lane = laneEnds.length;
    laneEnds[lane] = last;
    spans.push({transition: t, first, last, lane});
  }
  return spans;
}

// One line describing `t`, e.g. 'OPEN · played · 527ms'.
export function transitionLabel(t: Transition): string {
  const start = transitionStart(t);
  const end = transitionEnd(t);
  const parts = [transitionTypeName(t.type), t.status];
  if (start !== undefined && end !== undefined) {
    parts.push(Duration.humanise(end - start));
  }
  return parts.join(' · ');
}

// `start` relative to `ts`, e.g. '125ms ago'.
function formatRelativeTime(start: bigint, ts: bigint): string {
  const diff = ts - start;
  return diff >= 0n
    ? `${Duration.humanise(diff)} ago`
    : `in ${Duration.humanise(-diff)}`;
}

function simpleClassName(name: string): string {
  const dot = name.lastIndexOf('.');
  return dot >= 0 ? name.slice(dot + 1) : name;
}

// The properties of `t`, with the Mode and Change flags of `participant`
// (the selected layer's), as of snapshot time `ts`.
export function transitionRows(
  t: Transition,
  participant: TransitionParticipant | undefined,
  ts: bigint,
  layerName: (layerId: number) => string | undefined,
): PropRow[] {
  const b = new PropRowsBuilder();
  b.text('Type', transitionTypeName(t.type), {title: `${t.type}`});
  if (participant !== undefined) {
    b.text('Mode', transitionModeName(participant.mode));
  }
  const start = transitionStart(t);
  const end = transitionEnd(t);
  const isPlaying =
    start !== undefined && start <= ts && (end === undefined || ts <= end);
  b.text('Status', isPlaying ? 'playing' : t.status);
  if (start !== undefined) {
    b.text('Started', formatRelativeTime(start, ts));
  }
  if (start !== undefined && end !== undefined) {
    const durNs = end - start;
    b.text('Duration', Duration.humanise(durNs >= 0n ? durNs : 0n));
  }
  if (t.handler !== undefined) {
    b.text('Handler', t.handler);
  }
  if (t.flags !== 0) {
    b.text('Flags', transitionFlagNames(t.flags).join(' | '), {
      title: `0x${(t.flags >>> 0).toString(16)}`,
    });
  }
  if (participant !== undefined && participant.flags !== 0) {
    b.text('Change flags', changeFlagNames(participant.flags).join(' | '), {
      title: `0x${(participant.flags >>> 0).toString(16)}`,
    });
  }
  if (t.participants.length > 0) {
    b.links(
      'Participants',
      t.participants.map((p) => {
        const name = layerName(p.layerId);
        const text = `${name ?? '#' + p.layerId} · ${transitionModeName(p.mode)}`;
        return {
          text,
          target: {kind: 'layer', layerId: p.layerId},
        };
      }),
    );
  }
  b.text('Id', `${t.id}`, {mono: true});
  return b.build();
}

// All transitions, by start time.
export async function queryTransitions(engine: Engine): Promise<Transition[]> {
  const [transitionsRes, participantsRes] = await Promise.all([
    engine.query(`
      SELECT
        t.transition_id,
        t.transition_type,
        t.status,
        t.flags,
        t.send_time_ns,
        t.dispatch_time_ns,
        t.finish_time_ns,
        t.shell_abort_time_ns,
        t.wm_abort_time_ns,
        t.merge_time_ns,
        t.start_transaction_id,
        t.finish_transaction_id,
        h.handler_name
      FROM __intrinsic_window_manager_shell_transitions t
      LEFT JOIN (
        SELECT handler_id, min(handler_name) AS handler_name
        FROM __intrinsic_window_manager_shell_transition_handlers
        GROUP BY handler_id
      ) h ON h.handler_id = t.handler
      ORDER BY coalesce(t.dispatch_time_ns, t.send_time_ns, t.ts)
      LIMIT 10000
    `),
    engine.query(`
      SELECT transition_id, layer_id, mode, flags
      FROM __intrinsic_window_manager_shell_transition_participants
    `),
  ]);

  const participantsByTransition = new Map<number, TransitionParticipant[]>();
  const pIt = participantsRes.iter({
    transition_id: NUM,
    layer_id: NUM,
    mode: NUM,
    flags: NUM,
  });
  for (; pIt.valid(); pIt.next()) {
    let list = participantsByTransition.get(pIt.transition_id);
    if (list === undefined) {
      list = [];
      participantsByTransition.set(pIt.transition_id, list);
    }
    list.push({
      layerId: pIt.layer_id,
      mode: pIt.mode,
      flags: pIt.flags,
    });
  }

  const out: Transition[] = [];
  const tIt = transitionsRes.iter({
    transition_id: NUM,
    transition_type: NUM_NULL,
    status: STR_NULL,
    flags: NUM_NULL,
    send_time_ns: LONG_NULL,
    dispatch_time_ns: LONG_NULL,
    finish_time_ns: LONG_NULL,
    shell_abort_time_ns: LONG_NULL,
    wm_abort_time_ns: LONG_NULL,
    merge_time_ns: LONG_NULL,
    start_transaction_id: LONG_NULL,
    finish_transaction_id: LONG_NULL,
    handler_name: STR_NULL,
  });

  for (; tIt.valid(); tIt.next()) {
    const abortTs =
      tIt.shell_abort_time_ns ?? tIt.wm_abort_time_ns ?? undefined;
    out.push({
      id: tIt.transition_id,
      type: tIt.transition_type ?? 0,
      status: tIt.status ?? '',
      handler:
        tIt.handler_name !== null
          ? simpleClassName(tIt.handler_name)
          : undefined,
      flags: tIt.flags ?? 0,
      sendTs: tIt.send_time_ns ?? undefined,
      dispatchTs: tIt.dispatch_time_ns ?? undefined,
      finishTs: tIt.finish_time_ns ?? undefined,
      abortTs,
      mergeTs: tIt.merge_time_ns ?? undefined,
      startTransactionId: tIt.start_transaction_id ?? undefined,
      finishTransactionId: tIt.finish_transaction_id ?? undefined,
      participants: participantsByTransition.get(tIt.transition_id) ?? [],
    });
  }

  return out;
}
