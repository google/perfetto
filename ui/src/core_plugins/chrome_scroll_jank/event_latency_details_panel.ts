// Copyright (C) 2023 The Android Open Source Project
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
import {Duration, duration, Time, time} from '../../base/time';
import {hasArgs, renderArguments} from '../../frontend/slice_args';
import {renderDetails} from '../../frontend/slice_details';
import {
  getDescendantSliceTree,
  getSlice,
  SliceDetails,
  SliceTreeNode,
} from '../../trace_processor/sql_utils/slice';
import {
  asSliceSqlId,
  SliceSqlId,
} from '../../trace_processor/sql_utils/core_types';
import {
  ColumnDescriptor,
  Table,
  TableData,
  widgetColumn,
} from '../../widgets/table';
import {TreeTable, TreeTableAttrs} from '../../frontend/widgets/treetable';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {MultiParagraphText, TextParagraph} from '../../widgets/text_paragraph';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  EventLatencyCauseThreadTracks,
  EventLatencyStage,
  getCauseLink,
  getEventLatencyCauseTracks,
  getScrollJankCauseStage,
} from './scroll_jank_cause_link_utils';
import {ScrollJankCauseMap} from './scroll_jank_cause_map';
import {sliceRef} from '../../frontend/widgets/slice';
import {JANKS_TRACK_URI, renderSliceRef} from './selection_utils';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';

// Given a node in the slice tree, return a path from root to it.
function getPath(slice: SliceTreeNode): string[] {
  const result: string[] = [];
  let node: SliceTreeNode | undefined = slice;
  while (node.parent !== undefined) {
    result.push(node.name);
    node = node.parent;
  }
  return result.reverse();
}

// Given a slice tree node and a path, find the node following the path from
// the given slice, or `undefined` if not found.
function findSliceInTreeByPath(
  slice: SliceTreeNode | undefined,
  path: string[],
): SliceTreeNode | undefined {
  if (slice === undefined) {
    return undefined;
  }
  let result = slice;
  for (const segment of path) {
    let found = false;
    for (const child of result.children) {
      if (child.name === segment) {
        found = true;
        result = child;
        break;
      }
    }
    if (!found) {
      return undefined;
    }
  }
  return result;
}

function durationDelta(value: duration, base?: duration): string {
  if (base === undefined) {
    return 'NULL';
  }
  const delta = value - base;
  return `${delta > 0 ? '+' : ''}${Duration.humanise(delta)}`;
}

export class EventLatencySliceDetailsPanel implements TrackEventDetailsPanel {
  private name = '';
  private topEventLatencyId: SliceSqlId | undefined = undefined;

  private sliceDetails?: SliceDetails;
  private jankySlice?: {
    ts: time;
    dur: duration;
    id: number;
    causeOfJank: string;
  };

  // Whether this stage has caused jank. This is also true for top level
  // EventLatency slices where a descendant is a cause of jank.
  private isJankStage = false;

  // For top level EventLatency slices - if any descendant is a cause of jank,
  // this field stores information about that descendant slice. Otherwise, this
  // is stores information about the current stage;
  private relevantThreadStage: EventLatencyStage | undefined;
  private relevantThreadTracks: EventLatencyCauseThreadTracks[] = [];
  // Stages tree for the current EventLatency.
  private eventLatencyBreakdown?: SliceTreeNode;
  // Stages tree for the next EventLatency.
  private nextEventLatencyBreakdown?: SliceTreeNode;
  // Stages tree for the prev EventLatency.
  private prevEventLatencyBreakdown?: SliceTreeNode;

  private tracksByTrackId: Map<number, string>;

  constructor(
    private readonly trace: Trace,
    private readonly id: number,
  ) {
    this.tracksByTrackId = new Map<number, string>();
    this.trace.tracks.getAllTracks().forEach((td) => {
      td.tags?.trackIds?.forEach((trackId) => {
        this.tracksByTrackId.set(trackId, td.uri);
      });
    });
  }

  async load() {
    const queryResult = await this.trace.engine.query(`
      SELECT
        name
      FROM slice
      WHERE id = ${this.id}
      `);

    const iter = queryResult.firstRow({
      name: STR,
    });

    this.name = iter.name;

    await this.loadSlice();
    await this.loadJankSlice();
    await this.loadRelevantThreads();
    await this.loadEventLatencyBreakdown();
  }

  async loadSlice() {
    this.sliceDetails = await getSlice(
      this.trace.engine,
      asSliceSqlId(this.id),
    );
    this.trace.scheduleFullRedraw();
  }

  async loadJankSlice() {
    if (!this.sliceDetails) return;
    // Get the id for the top-level EventLatency slice (this or parent), as
    // this id is used in the ScrollJankV3 track to identify the corresponding
    // janky interval.
    if (this.sliceDetails.name === 'EventLatency') {
      this.topEventLatencyId = this.sliceDetails.id;
    } else {
      this.topEventLatencyId = asSliceSqlId(
        await this.getOldestAncestorSliceId(),
      );
    }

    const it = (
      await this.trace.engine.query(`
      SELECT ts, dur, id, cause_of_jank as causeOfJank
      FROM chrome_janky_frame_presentation_intervals
      WHERE event_latency_id = ${this.topEventLatencyId}`)
    ).iter({
      id: NUM,
      ts: LONG,
      dur: LONG,
      causeOfJank: STR,
    });

    if (it.valid()) {
      this.jankySlice = {
        id: it.id,
        ts: Time.fromRaw(it.ts),
        dur: Duration.fromRaw(it.dur),
        causeOfJank: it.causeOfJank,
      };
    }
  }

  async loadRelevantThreads() {
    if (!this.sliceDetails) return;
    if (!this.topEventLatencyId) return;

    // Relevant threads should only be available on a "Janky" EventLatency
    // slice to allow the user to jump to the possible cause of jank.
    if (this.sliceDetails.name === 'EventLatency' && !this.jankySlice) return;

    const possibleScrollJankStage = await getScrollJankCauseStage(
      this.trace.engine,
      this.topEventLatencyId,
    );
    if (this.sliceDetails.name === 'EventLatency') {
      this.isJankStage = true;
      this.relevantThreadStage = possibleScrollJankStage;
    } else {
      if (
        possibleScrollJankStage &&
        this.sliceDetails.name === possibleScrollJankStage.name
      ) {
        this.isJankStage = true;
      }
      this.relevantThreadStage = {
        name: this.sliceDetails.name,
        eventLatencyId: this.topEventLatencyId,
        ts: this.sliceDetails.ts,
        dur: this.sliceDetails.dur,
      };
    }

    if (this.relevantThreadStage) {
      this.relevantThreadTracks = await getEventLatencyCauseTracks(
        this.trace.engine,
        this.relevantThreadStage,
      );
    }
  }

  async loadEventLatencyBreakdown() {
    if (this.topEventLatencyId === undefined) {
      return;
    }
    this.eventLatencyBreakdown = await getDescendantSliceTree(
      this.trace.engine,
      this.topEventLatencyId,
    );

    // TODO(altimin): this should only consider EventLatencies within the same scroll.
    const prevEventLatency = (
      await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE chrome.event_latency;
      SELECT
        id
      FROM chrome_event_latencies
      WHERE event_type IN (
        'FIRST_GESTURE_SCROLL_UPDATE',
        'GESTURE_SCROLL_UPDATE',
        'INERTIAL_GESTURE_SCROLL_UPDATE')
      AND is_presented
      AND id < ${this.topEventLatencyId}
      ORDER BY id DESC
      LIMIT 1
      ;
    `)
    ).maybeFirstRow({id: NUM});
    if (prevEventLatency !== undefined) {
      this.prevEventLatencyBreakdown = await getDescendantSliceTree(
        this.trace.engine,
        asSliceSqlId(prevEventLatency.id),
      );
    }

    const nextEventLatency = (
      await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE chrome.event_latency;
      SELECT
        id
      FROM chrome_event_latencies
      WHERE event_type IN (
        'FIRST_GESTURE_SCROLL_UPDATE',
        'GESTURE_SCROLL_UPDATE',
        'INERTIAL_GESTURE_SCROLL_UPDATE')
      AND is_presented
      AND id > ${this.topEventLatencyId}
      ORDER BY id DESC
      LIMIT 1;
    `)
    ).maybeFirstRow({id: NUM});
    if (nextEventLatency !== undefined) {
      this.nextEventLatencyBreakdown = await getDescendantSliceTree(
        this.trace.engine,
        asSliceSqlId(nextEventLatency.id),
      );
    }
  }

  private getRelevantLinks(): m.Child {
    if (!this.sliceDetails) return undefined;

    // Relevant threads should only be available on a "Janky" EventLatency
    // slice to allow the user to jump to the possible cause of jank.
    if (
      this.sliceDetails.name === 'EventLatency' &&
      !this.relevantThreadStage
    ) {
      return undefined;
    }

    const name = this.relevantThreadStage
      ? this.relevantThreadStage.name
      : this.sliceDetails.name;
    const ts = this.relevantThreadStage
      ? this.relevantThreadStage.ts
      : this.sliceDetails.ts;
    const dur = this.relevantThreadStage
      ? this.relevantThreadStage.dur
      : this.sliceDetails.dur;
    const stageDetails = ScrollJankCauseMap.getEventLatencyDetails(name);
    if (stageDetails === undefined) return undefined;

    const childWidgets: m.Child[] = [];
    childWidgets.push(m(TextParagraph, {text: stageDetails.description}));

    interface RelevantThreadRow {
      description: string;
      tracks: EventLatencyCauseThreadTracks;
      ts: time;
      dur: duration;
    }

    const columns: ColumnDescriptor<RelevantThreadRow>[] = [
      widgetColumn<RelevantThreadRow>('Relevant Thread', (x) =>
        getCauseLink(this.trace, x.tracks, this.tracksByTrackId, x.ts, x.dur),
      ),
      widgetColumn<RelevantThreadRow>('Description', (x) => {
        if (x.description === '') {
          return x.description;
        } else {
          return m(TextParagraph, {text: x.description});
        }
      }),
    ];

    const trackLinks: RelevantThreadRow[] = [];

    for (let i = 0; i < this.relevantThreadTracks.length; i++) {
      const track = this.relevantThreadTracks[i];
      let description = '';
      if (i == 0 || track.thread != this.relevantThreadTracks[i - 1].thread) {
        description = track.causeDescription;
      }
      trackLinks.push({
        description: description,
        tracks: this.relevantThreadTracks[i],
        ts: ts,
        dur: dur,
      });
    }

    const tableData = new TableData(trackLinks);

    if (trackLinks.length > 0) {
      childWidgets.push(
        m(Table, {
          data: tableData,
          columns: columns,
        }),
      );
    }

    return m(
      Section,
      {title: this.isJankStage ? `Jank Cause: ${name}` : name},
      childWidgets,
    );
  }

  private async getOldestAncestorSliceId(): Promise<number> {
    let eventLatencyId = -1;
    if (!this.sliceDetails) return eventLatencyId;
    const queryResult = await this.trace.engine.query(`
      SELECT
        id
      FROM ancestor_slice(${this.sliceDetails.id})
      WHERE name = 'EventLatency'
    `);

    const it = queryResult.iter({
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      eventLatencyId = it.id;
      break;
    }

    return eventLatencyId;
  }

  private getLinksSection(): m.Child {
    return m(
      Section,
      {title: 'Quick links'},
      m(
        Tree,
        m(TreeNode, {
          left: this.sliceDetails
            ? sliceRef(
                this.sliceDetails,
                'EventLatency in context of other Input events',
              )
            : 'EventLatency in context of other Input events',
          right: this.sliceDetails ? '' : 'N/A',
        }),
        this.jankySlice &&
          m(TreeNode, {
            left: renderSliceRef({
              trace: this.trace,
              id: this.jankySlice.id,
              trackUri: JANKS_TRACK_URI,
              title: this.jankySlice.causeOfJank,
            }),
          }),
      ),
    );
  }

  private getBreakdownSection(): m.Child {
    if (this.eventLatencyBreakdown === undefined) {
      return undefined;
    }

    const attrs: TreeTableAttrs<SliceTreeNode> = {
      rows: [this.eventLatencyBreakdown],
      getChildren: (slice) => slice.children,
      columns: [
        {name: 'Name', getData: (slice) => slice.name},
        {name: 'Duration', getData: (slice) => Duration.humanise(slice.dur)},
        {
          name: 'vs prev',
          getData: (slice) =>
            durationDelta(
              slice.dur,
              findSliceInTreeByPath(
                this.prevEventLatencyBreakdown,
                getPath(slice),
              )?.dur,
            ),
        },
        {
          name: 'vs next',
          getData: (slice) =>
            durationDelta(
              slice.dur,
              findSliceInTreeByPath(
                this.nextEventLatencyBreakdown,
                getPath(slice),
              )?.dur,
            ),
        },
      ],
    };

    return m(
      Section,
      {
        title: 'EventLatency Stage Breakdown',
      },
      m(TreeTable<SliceTreeNode>, attrs),
    );
  }

  private getDescriptionText(): m.Child {
    return m(
      MultiParagraphText,
      m(TextParagraph, {
        text: `EventLatency tracks the latency of handling a given input event
                 (Scrolls, Touches, Taps, etc). Ideally from when the input was
                 read by the hardware to when it was reflected on the screen.`,
      }),
      m(TextParagraph, {
        text: `Note however the concept of coalescing or terminating early. This
               occurs when we receive multiple events or handle them quickly by
               converting them into a different event. Such as a TOUCH_MOVE
               being converted into a GESTURE_SCROLL_UPDATE type, or a multiple
               GESTURE_SCROLL_UPDATE events being formed into a single frame at
               the end of the RendererCompositorQueuingDelay.`,
      }),
      m(TextParagraph, {
        text: `*Important:* On some platforms (MacOS) we do not get feedback on
               when something is presented on the screen so the timings are only
               accurate for what we know on a given platform.`,
      }),
    );
  }

  render() {
    if (this.sliceDetails) {
      const slice = this.sliceDetails;

      const rightSideWidgets: m.Child[] = [];
      rightSideWidgets.push(
        m(
          Section,
          {title: 'Description'},
          m('.div', this.getDescriptionText()),
        ),
      );

      const stageWidget = this.getRelevantLinks();
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      if (stageWidget) {
        rightSideWidgets.push(stageWidget);
      }
      rightSideWidgets.push(this.getLinksSection());
      rightSideWidgets.push(this.getBreakdownSection());

      return m(
        DetailsShell,
        {
          title: 'Slice',
          description: this.name,
        },
        m(
          GridLayout,
          m(
            GridLayoutColumn,
            renderDetails(this.trace, slice),
            hasArgs(slice.args) &&
              m(
                Section,
                {title: 'Arguments'},
                m(Tree, renderArguments(this.trace, slice.args)),
              ),
          ),
          m(GridLayoutColumn, rightSideWidgets),
        ),
      );
    } else {
      return m(DetailsShell, {title: 'Slice', description: 'Loading...'});
    }
  }
}
