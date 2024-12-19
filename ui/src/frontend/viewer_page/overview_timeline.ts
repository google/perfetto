// Copyright (C) 2024 The Android Open Source Project
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
import {TraceImpl} from '../../core/trace_impl';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {VirtualOverlayCanvas} from '../../widgets/virtual_overlay_canvas';
import {raf} from '../../core/raf_scheduler';

export interface OverviewTimelineAttrs {
  readonly trace: TraceImpl;
  readonly className?: string;
}

export class OverviewTimeline
  implements m.ClassComponent<OverviewTimelineAttrs>
{
  private readonly overviewTimeline: OverviewTimelinePanel;

  constructor({attrs}: m.Vnode<OverviewTimelineAttrs>) {
    this.overviewTimeline = new OverviewTimelinePanel(attrs.trace);
  }

  view({attrs}: m.Vnode<OverviewTimelineAttrs>) {
    return m(
      VirtualOverlayCanvas,
      {
        className: attrs.className,
        onCanvasRedraw: ({ctx, virtualCanvasSize}) => {
          this.overviewTimeline.renderCanvas(ctx, virtualCanvasSize);
        },
        onCanvasCreate: (overlay) => {
          overlay.trash.use(
            raf.addCanvasRedrawCallback(() => overlay.redrawCanvas()),
          );
        },
      },
      this.overviewTimeline.render(),
    );
  }
}
