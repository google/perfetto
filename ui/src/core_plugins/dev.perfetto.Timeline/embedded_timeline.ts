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

import './embedded_timeline.scss';
import m from 'mithril';
import {classNames} from '../../base/classnames';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {Time, type time} from '../../base/time';
import {AppImpl} from '../../core/app_impl';
import {TimelineImpl} from '../../core/timeline';
import type {TraceImpl} from '../../core/trace_impl';
import type {TrackNode} from '../../public/workspace';
import {CurrentSelectionTab} from './current_selection_tab';
import {TimelineHeader} from './timeline_header';
import {TrackTreeView} from './track_tree_view';

export interface EmbeddedTimelineWindow {
  readonly start: time;
  readonly end: time;
}

export interface EmbeddedTimelineOpts {
  readonly rootNode: TrackNode;
  readonly initialWindow?: EmbeddedTimelineWindow;
  // Change this key to re-initialize the visible window (e.g., process change).
  readonly windowKey?: string;
  readonly showSelectionDetails?: boolean;
  readonly className?: string;
}

interface EmbeddedTimelineComponentAttrs extends EmbeddedTimelineOpts {
  readonly trace: TraceImpl;
}

function makeTimelineForWindow(
  trace: TraceImpl,
  win?: EmbeddedTimelineWindow,
): TimelineImpl {
  const app = AppImpl.instance;
  const timeline = new TimelineImpl(
    trace.traceInfo,
    app.timestampFormat,
    app.durationPrecision,
    app.timezoneOverride,
  );
  if (win !== undefined) {
    const start = Time.min(win.start, win.end);
    const end = Time.max(win.start, win.end);
    const dur = end - start;
    const margin = dur > 0n ? dur / 10n : 1_000_000n;
    const clampedStart = Time.max(
      trace.traceInfo.start,
      Time.fromRaw(start - margin),
    );
    const clampedEnd = Time.min(
      trace.traceInfo.end,
      Time.fromRaw(end + margin),
    );
    if (clampedEnd > clampedStart) {
      timeline.setVisibleWindow(
        HighPrecisionTimeSpan.fromTime(clampedStart, clampedEnd),
      );
    }
  }
  return timeline;
}

// A self-contained, focused timeline with its own pan/zoom state, compact
// axis header, curated track tree, and optional split details panel below.
export class EmbeddedTimeline implements m.ClassComponent<EmbeddedTimelineComponentAttrs> {
  private timeline?: TimelineImpl;
  private lastWindowKey?: string;
  private lastTraceUuid?: string;

  view({attrs}: m.CVnode<EmbeddedTimelineComponentAttrs>): m.Children {
    const {
      trace,
      rootNode,
      initialWindow,
      windowKey,
      showSelectionDetails = true,
      className,
    } = attrs;
    if (
      this.timeline === undefined ||
      this.lastTraceUuid !== trace.traceInfo.uuid ||
      this.lastWindowKey !== windowKey
    ) {
      this.timeline = makeTimelineForWindow(trace, initialWindow);
      this.lastTraceUuid = trace.traceInfo.uuid;
      this.lastWindowKey = windowKey;
    }

    const hasSelection =
      showSelectionDetails && trace.selection.selection.kind !== 'empty';

    return m(
      '.pf-embedded-timeline-host',
      {className: classNames(className)},
      m('.pf-embedded-timeline', [
        m(TimelineHeader, {
          trace,
          timeline: this.timeline,
          compact: true,
          className: 'pf-embedded-timeline__header',
        }),
        m(TrackTreeView, {
          trace,
          timeline: this.timeline,
          rootNode,
          canPinTracks: false,
          className: 'pf-embedded-timeline__tracks',
        }),
      ]),
      hasSelection &&
        m(
          '.pf-embedded-timeline-host__details',
          m(CurrentSelectionTab, {trace}),
        ),
    );
  }
}
