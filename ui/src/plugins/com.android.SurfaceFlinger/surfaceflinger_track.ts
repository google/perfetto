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

// Per-display timeline track of SurfaceFlinger layer snapshots (one incomplete
// slice per snapshot, spanning to the next, like the Screenshots track). It
// gives the viewer a timeline presence: selecting a snapshot shows a compact
// summary and a link that opens the full SurfaceFlinger page at that moment.

import m from 'mithril';
import {Time} from '../../base/time';
import {Timestamp} from '../../components/widgets/timestamp';
import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {rectsOptionsFrom} from './surfaceflinger_controls';
import {type SfLayer, sqlInt} from './surfaceflinger_data';
import {SfRectsView} from './surfaceflinger_rects';
import {SURFACEFLINGER_ROUTE} from './surfaceflinger_route';
import type {SurfaceFlingerSession} from './surfaceflinger_session';

// Compact peek shown when a snapshot is selected on a display's timeline track:
// the counts + top visible layers for that display plus a prominent button into
// the full-screen viewer, kept in sync via load(). All the detail
// (rects/hierarchy/properties) lives on the page.
class SfSnapshotPanel implements TrackEventDetailsPanel {
  private ts?: bigint;
  private displayName = 'Display';
  private index = 0; // snapshot index, to match the page's "N / total"
  private total = 0;
  private nLayers = 0;
  private nVisible = 0;
  private topLayers: string[] = [];
  // This display's layers at this snapshot, captured at load() so the inline
  // layout preview is stable even if the shared session later moves on.
  private layers: SfLayer[] = [];

  constructor(
    private readonly session: SurfaceFlingerSession,
    private readonly displayId: string,
  ) {}

  async load(sel: TrackEventSelection): Promise<void> {
    this.ts = sel.ts;
    // Sync the shared session to this track's display + snapshot, so the page
    // opens here and the preview is scoped to this display.
    if (this.session.displayId !== this.displayId) {
      await this.session.setDisplay(this.displayId);
    }
    await this.session.setNearestTs(sel.ts);
    this.displayName = this.session.displayName;
    this.index = this.session.index;
    this.total = this.session.snapshots.length;
    const layers = this.session.displayLayers();
    this.layers = layers;
    this.nLayers = layers.length;
    const visible = layers.filter((l) => l.isVisible);
    this.nVisible = visible.length;
    // Top-most visible layers (highest draw depth = front).
    this.topLayers = visible
      .filter((l) => l.rect)
      .sort((a, b) => (b.drawDepth ?? 0) - (a.drawDepth ?? 0))
      .slice(0, 6)
      .map((l) => l.name.replace(/#\d+$/, '').trim());
    m.redraw();
  }

  render(): m.Children {
    return m(
      DetailsShell,
      {
        title: 'SurfaceFlinger snapshot',
        description: `${this.displayName} — ${this.nVisible}/${this.nLayers} layers visible`,
        buttons: m(
          'a',
          {
            href: `#!${SURFACEFLINGER_ROUTE}`,
            title: 'Open the full viewer at this snapshot',
          },
          m(Button, {
            label: 'Open in SurfaceFlinger viewer',
            icon: 'open_in_full',
            intent: Intent.Primary,
          }),
        ),
      },
      m(
        GridLayout,
        m(
          Section,
          {title: 'Snapshot'},
          m(Tree, [
            m(TreeNode, {
              left: 'Snapshot',
              right: `${this.index + 1} / ${this.total}`,
            }),
            this.ts !== undefined &&
              m(TreeNode, {
                left: 'Timestamp',
                right: m(Timestamp, {
                  trace: this.session.trace,
                  ts: Time.fromRaw(this.ts),
                }),
              }),
            m(TreeNode, {left: 'Layers', right: `${this.nLayers}`}),
            m(TreeNode, {left: 'Visible', right: `${this.nVisible}`}),
            m(
              TreeNode,
              {left: 'Top layers', right: `${this.topLayers.length}`},
              this.topLayers.map((n) => m(TreeNode, {left: n})),
            ),
          ]),
        ),
        // Inline layout preview (like the VideoFrames panel shows the frame).
        // No controls here — it respects the view options set on the page (shared
        // session state) to keep the panel uncluttered. Clicking a rect or its
        // label still selects the layer.
        m(
          Section,
          {title: 'Layout'},
          this.layers.length === 0
            ? m('span', 'No layers.')
            : m(SfRectsView, {
                layers: this.layers,
                selectedRowId: this.session.selectedRowId,
                hiddenLayerIds: this.session.hiddenLayerIds,
                pinnedLayerIds: this.session.pinnedLayerIds,
                onSelect: (rowId) => void this.session.selectLayer(rowId),
                options: rectsOptionsFrom(this.session.options),
              }),
        ),
      ),
    );
  }
}

export function createSurfaceFlingerTrack(
  trace: Trace,
  uri: string,
  displayId: string,
  session: SurfaceFlingerSession,
) {
  // One track per display: show each snapshot that includes this display as an
  // incomplete slice spanning to the next (dur = -1, like the Screenshots /
  // VideoFrames tracks), labelled with its frame number within this display. The
  // name doubles as the per-frame colorization seed.
  const src = `
    SELECT
      id,
      ts,
      -1 AS dur,
      0 AS depth,
      'Frame ' || (ROW_NUMBER() OVER (ORDER BY ts)) AS name
    FROM __intrinsic_surfaceflinger_layers_snapshot
    WHERE id IN (
      SELECT snapshot_id FROM __intrinsic_surfaceflinger_display
      WHERE display_id = ${sqlInt(displayId)}
    )
  `;
  const panel = new SfSnapshotPanel(session, displayId);
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {id: NUM, ts: LONG, dur: LONG, name: STR, depth: NUM},
      src,
    }),
    colorizer: (row) => materialColorScheme(row.name),
    detailsPanel: () => panel,
  });
}
