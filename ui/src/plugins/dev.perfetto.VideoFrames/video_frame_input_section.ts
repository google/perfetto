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
import {AsyncMemo} from '../../base/async_memo';
import {Duration, Time, type duration, type time} from '../../base/time';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {Trace} from '../../public/trace';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

// MotionEvent action names, by masked action.
const ACTION_NAMES: ReadonlyMap<number, string> = new Map([
  [0, 'DOWN'],
  [1, 'UP'],
  [5, 'POINTER_DOWN'],
  [6, 'POINTER_UP'],
]);

// One window (input channel) an input event was dispatched to, and the video
// frame that first showed that window's response.
export interface InputReceiver {
  readonly process?: string;
  // Short name of the window, telling apart a process's windows.
  readonly channel?: string;
  readonly dispatchSliceId?: number;
  readonly receiveSliceId?: number;
  readonly responseFrameId?: number;
  // False when the response frame is the first one after the response was
  // presented, rather than the composite that presented it.
  readonly isResponseExact: boolean;
  readonly inputToPhoton?: duration;
}

// An input event linked to the video: the frame on screen when it happened,
// and per receiving window, the frame that showed the response.
export interface VideoInputEvent {
  readonly motionEventId: number;
  readonly ts: time;
  readonly action: string;
  readonly visibleFrameId?: number;
  readonly receivers: ReadonlyArray<InputReceiver>;
}

// A row of android_input_event_video_frames: one per (motion event,
// channel); a single row with NULL channel columns without atrace.
export interface InputVideoRow {
  readonly motionEventId: number;
  readonly eventTs: bigint;
  readonly actionMasked: number;
  readonly processName: string | null;
  // The input channel, normalized as android_input_events does.
  readonly channel: string | null;
  readonly visibleFrameId: number | null;
  readonly responseFrameId: number | null;
  readonly isResponseExact: number | null;
  readonly inputToPhotonDur: bigint | null;
  readonly dispatchSliceId: number | null;
  readonly receiveSliceId: number | null;
}

// A short window name from a normalized input channel: the activity or class
// name without its package, e.g. "NexusLauncherActivity" for
// "com.google.android.apps.nexuslauncher/....NexusLauncherActivity". Omitted
// when it just repeats the process name.
export function shortChannelName(
  channel: string,
  process: string | undefined,
): string | undefined {
  if (channel === process) return undefined;
  // Monitors read best as is, e.g. "[Gesture Monitor] swipe-up".
  if (channel.startsWith('[')) return channel;
  const component = channel.split('/').pop() ?? channel;
  if (component.includes(' ')) return component;
  return component.split('.').pop();
}

// Collapses the per-channel rows into one entry per motion event, keeping
// row order, with a receiver per row.
export function groupInputRows(
  rows: ReadonlyArray<InputVideoRow>,
): VideoInputEvent[] {
  const byId = new Map<
    number,
    {head: Omit<VideoInputEvent, 'receivers'>; receivers: InputReceiver[]}
  >();
  for (const r of rows) {
    let entry = byId.get(r.motionEventId);
    if (entry === undefined) {
      const head = {
        motionEventId: r.motionEventId,
        ts: Time.fromRaw(r.eventTs),
        action: ACTION_NAMES.get(r.actionMasked) ?? `${r.actionMasked}`,
        visibleFrameId: r.visibleFrameId ?? undefined,
      };
      entry = {head, receivers: []};
      byId.set(r.motionEventId, entry);
    }
    const process = r.processName ?? undefined;
    entry.receivers.push({
      process,
      channel:
        r.channel === null ? undefined : shortChannelName(r.channel, process),
      dispatchSliceId: r.dispatchSliceId ?? undefined,
      receiveSliceId: r.receiveSliceId ?? undefined,
      responseFrameId: r.responseFrameId ?? undefined,
      isResponseExact: r.isResponseExact === 1,
      inputToPhoton: Duration.fromRaw(r.inputToPhotonDur ?? undefined),
    });
  }
  return Array.from(byId.values(), ({head, receivers}) => ({
    ...head,
    receivers,
  }));
}

// The "Input" section of a video frame's details: the touches made while the
// frame was on screen, each linking to the frame that showed the response,
// and the touches this frame responds to, linking back.
export class VideoFrameInputSection {
  private readonly trace: Trace;
  private readonly trackUri: string;
  private readonly displayId: number;
  private readonly eventsMemo = new AsyncMemo<VideoInputEvent[]>();

  constructor(trace: Trace, trackUri: string, displayId: number) {
    this.trace = trace;
    this.trackUri = trackUri;
    this.displayId = displayId;
  }

  // `enabled` is false while playing, so advancing frames fire no queries.
  render(frameId: number, enabled: boolean): m.Children {
    const events = this.eventsMemo.use({
      key: {frameId},
      enabled,
      compute: () => this.loadEvents(frameId),
    }).data;
    if (!enabled || events === undefined || events.length === 0) {
      return undefined;
    }
    const inFrame = events.filter((e) => e.visibleFrameId === frameId);
    const responses = events.filter((e) =>
      e.receivers.some((r) => r.responseFrameId === frameId),
    );
    return m(
      Section,
      {title: 'Input'},
      m(
        Tree,
        inFrame.map((e) => this.renderInput(e)),
        responses.map((e) => this.renderResponse(e, frameId)),
      ),
    );
  }

  private async loadEvents(frameId: number): Promise<VideoInputEvent[]> {
    const res = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE android.input_video;
      SELECT
        motion_event_id AS motionEventId,
        event_ts AS eventTs,
        action_masked AS actionMasked,
        process_name AS processName,
        _normalize_event_channel(event_channel) AS channel,
        visible_video_frame_id AS visibleFrameId,
        response_video_frame_id AS responseFrameId,
        is_response_exact AS isResponseExact,
        input_to_photon_dur AS inputToPhotonDur,
        dispatch_slice_id AS dispatchSliceId,
        receive_slice_id AS receiveSliceId
      FROM android_input_event_video_frames
      WHERE display_id = ${this.displayId}
        AND (visible_video_frame_id = ${frameId}
          OR response_video_frame_id = ${frameId})
      ORDER BY event_ts, motion_event_id,
        response_video_frame_id IS NULL, process_name
    `);
    const it = res.iter({
      motionEventId: NUM,
      eventTs: LONG,
      actionMasked: NUM,
      processName: STR_NULL,
      channel: STR_NULL,
      visibleFrameId: NUM_NULL,
      responseFrameId: NUM_NULL,
      isResponseExact: NUM_NULL,
      inputToPhotonDur: LONG_NULL,
      dispatchSliceId: NUM_NULL,
      receiveSliceId: NUM_NULL,
    });
    const rows: InputVideoRow[] = [];
    for (; it.valid(); it.next()) {
      rows.push({
        motionEventId: it.motionEventId,
        eventTs: it.eventTs,
        actionMasked: it.actionMasked,
        processName: it.processName,
        channel: it.channel,
        visibleFrameId: it.visibleFrameId,
        responseFrameId: it.responseFrameId,
        isResponseExact: it.isResponseExact,
        inputToPhotonDur: it.inputToPhotonDur,
        dispatchSliceId: it.dispatchSliceId,
        receiveSliceId: it.receiveSliceId,
      });
    }
    return groupInputRows(rows);
  }

  // A touch made while this frame was on screen: the windows that
  // responded, each with the frame showing its response, then the other
  // windows it went to (spies, monitors, ...) folded away.
  private renderInput(e: VideoInputEvent): m.Children {
    const responders = e.receivers.filter(
      (r) => r.responseFrameId !== undefined,
    );
    const others = e.receivers.filter(
      (r) => r.responseFrameId === undefined && r.process !== undefined,
    );
    const renderAll = (rs: ReadonlyArray<InputReceiver>) =>
      rs.map((r) =>
        this.renderReceiver(r, r.responseFrameId, 'Jump to response frame'),
      );
    return m(
      TreeNode,
      {
        left: e.action,
        right: m(Timestamp, {trace: this.trace, ts: e.ts}),
      },
      renderAll(responders),
      others.length > 0 &&
        m(
          TreeNode,
          {
            left:
              responders.length > 0
                ? `${others.length} other windows`
                : `${others.length} windows`,
            startsCollapsed: true,
          },
          renderAll(others),
        ),
    );
  }

  // A touch this frame shows a response to, with the windows that responded
  // in this frame, linking back to the frame on screen at the touch.
  private renderResponse(e: VideoInputEvent, frameId: number): m.Children {
    return m(
      TreeNode,
      {
        left: `Response to ${e.action}`,
        right: m(Timestamp, {trace: this.trace, ts: e.ts}),
      },
      e.receivers
        .filter((r) => r.responseFrameId === frameId)
        .map((r) =>
          this.renderReceiver(r, e.visibleFrameId, 'Jump to input frame'),
        ),
    );
  }

  // A receiving window: its dispatch/receive slices (with atrace input
  // slices), input-to-photon latency and a link to another video frame.
  private renderReceiver(
    r: InputReceiver,
    jumpFrameId: number | undefined,
    jumpLabel: string,
  ): m.Children {
    const latency =
      r.inputToPhoton !== undefined &&
      m(TreeNode, {
        left: r.isResponseExact
          ? 'Input to photon'
          : 'Input to photon (approx.)',
        right: m(DurationWidget, {trace: this.trace, dur: r.inputToPhoton}),
      });
    const jump =
      jumpFrameId !== undefined &&
      m(TreeNode, {
        left: m(
          Anchor,
          {icon: 'movie', onclick: () => this.selectFrame(jumpFrameId)},
          jumpLabel,
        ),
      });
    if (r.process === undefined) return [latency, jump];
    return m(
      TreeNode,
      {
        left: [
          r.process,
          r.channel !== undefined &&
            m('span.pf-video-frame-channel', ` ${r.channel}`),
        ],
        right: [
          this.renderSliceLink('Dispatch', r.dispatchSliceId),
          ' ',
          this.renderSliceLink('Receive', r.receiveSliceId),
        ],
      },
      latency,
      jump,
    );
  }

  private renderSliceLink(label: string, sliceId?: number): m.Children {
    if (sliceId === undefined) return undefined;
    return m(
      Anchor,
      {
        onclick: () =>
          this.trace.selection.selectSqlEvent('slice', sliceId, {
            scrollToSelection: true,
          }),
      },
      label,
    );
  }

  private selectFrame(frameId: number): void {
    this.trace.selection.selectTrackEvent(this.trackUri, frameId, {
      scrollToSelection: true,
    });
  }
}
