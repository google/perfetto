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
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {createSurfaceFlingerTrack} from './surfaceflinger_track';
import {SurfaceFlingerPage} from './surfaceflinger_page';
import {SURFACEFLINGER_ROUTE} from './surfaceflinger_route';
import {SurfaceFlingerSession} from './surfaceflinger_session';

// Viewer for SurfaceFlinger layer traces (the android.surfaceflinger.layers
// data source). Surfaced two ways onto one shared session: a per-display
// timeline track of layer snapshots, and a full-screen page (Surface/rects view
// + layer hierarchy + curated & full proto properties) reached from the sidebar
// or by opening a snapshot from the timeline.
export default class implements PerfettoPlugin {
  static readonly id = 'com.android.SurfaceFlinger';
  static readonly description =
    'SurfaceFlinger viewer: per-display layer snapshots on the timeline plus a ' +
    'full-screen rects/hierarchy/properties page. Inspired by the Android ' +
    'Winscope tool.';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const session = new SurfaceFlingerSession(ctx);
    await session.init();
    if (session.displays.length === 0) return;

    // Full-screen page + sidebar entry.
    ctx.pages.registerPage({
      route: SURFACEFLINGER_ROUTE,
      render: (subpage) => m(SurfaceFlingerPage, {session, subpage}),
    });
    ctx.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 35,
      text: 'SurfaceFlinger',
      href: `#!${SURFACEFLINGER_ROUTE}`,
      icon: 'layers',
    });

    // A nested "SurfaceFlinger" group with one track per display (real and
    // virtual — virtual displays such as a video-encoder/screen-recorder target
    // are part of what SurfaceFlinger actually composited, so they are shown).
    // Each track is scoped to its display's composition; the same displays
    // appear in the page's selector, so the timeline and page stay consistent.
    const group = new TrackNode({
      name: 'SurfaceFlinger',
      isSummary: true,
      sortOrder: -50,
    });
    for (const display of session.displays) {
      const uri = `/surfaceflinger_track/${display.displayId}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: createSurfaceFlingerTrack(
          ctx,
          uri,
          display.displayId,
          session,
        ),
      });
      const name = display.isVirtual
        ? `${display.displayName} (virtual)`
        : display.displayName;
      group.addChildInOrder(new TrackNode({uri, name}));
    }
    ctx.defaultWorkspace.addChildInOrder(group);
  }
}
