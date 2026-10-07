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

// "Show in timeline" deep-linking, shared by the Memscope overview panels.

import m from 'mithril';
import {Icons} from '../../../../base/semantic_icons';
import type {time} from '../../../../base/time';
import type {Trace} from '../../../../public/trace';
import {Anchor} from '../../../../widgets/anchor';

// Finds the workspace track for `upid` whose kinds satisfy `kindMatch` (e.g.
// the smaps, java-heap-graph or heapprofd track), or undefined if absent.
export function findProcessTrack(
  trace: Trace,
  upid: number | undefined,
  kindMatch: (kind: string) => boolean,
) {
  if (upid === undefined) return undefined;
  return trace.defaultWorkspace.flatTracks.find((t) => {
    if (t.uri === undefined) return false;
    const track = trace.tracks.getTrack(t.uri);
    if (track?.tags?.upid !== upid) return false;
    return (track?.tags?.kinds ?? []).some(kindMatch);
  });
}

// "Show in timeline" deep-link: selects `trackNode` and switches to the
// timeline. Disabled (greyed out) when there's no matching track.
export function showInTimelineLink(
  trace: Trace,
  uri: string | undefined,
  eventId: number,
): m.Child {
  return m(
    Anchor,
    {
      disabled: uri === undefined,
      icon: Icons.UpdateSelection,
      onclick: () => {
        if (uri === undefined) return;
        trace.selection.selectTrackEvent(uri, eventId, {
          clearSearch: true,
          scrollToSelection: true,
        });
        trace.navigate('#!/viewer');
      },
    },
    'Show in timeline',
  );
}

// "Show in timeline" link for data aggregated over several track events.
// Creates an area selection so timeline aggregation tabs (e.g. the native heap
// flamegraph) combine every event in the displayed range.
export function showAreaInTimelineLink(
  trace: Trace,
  uri: string | undefined,
  start: time | undefined,
  end: time | undefined,
): m.Child {
  const disabled =
    uri === undefined || start === undefined || end === undefined;
  return m(
    Anchor,
    {
      disabled,
      icon: Icons.UpdateSelection,
      onclick: () => {
        if (uri === undefined || start === undefined || end === undefined) {
          return;
        }
        trace.selection.selectArea(
          {start, end, trackUris: [uri]},
          {clearSearch: true, scrollToSelection: true},
        );
        trace.navigate('#!/viewer');
      },
    },
    'Show in timeline',
  );
}
