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

import {exists} from '../../base/utils';
import {NUM} from '../../common/query_result';
import {raf} from '../../core/raf_scheduler';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {renderArguments} from '../../frontend/slice_args';
import {renderDetails} from '../../frontend/slice_details';
import {getSlice, SliceDetails, sliceRef} from '../../frontend/sql/slice';
import {asSliceSqlId} from '../../frontend/sql_types';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {MultiParagraphText, TextParagraph} from '../../widgets/text_paragraph';
import {Tree, TreeNode} from '../../widgets/tree';

import {
  getScrollJankSlices,
  getSliceForTrack,
  ScrollJankSlice,
} from './scroll_jank_slice';
import {ScrollJankV3Track} from './scroll_jank_v3_track';

export class EventLatencySliceDetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'dev.perfetto.EventLatencySliceDetailsPanel';

  private loaded = false;

  private sliceDetails?: SliceDetails;
  private jankySlice?: ScrollJankSlice;

  static create(args: NewBottomTabArgs): EventLatencySliceDetailsPanel {
    return new EventLatencySliceDetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    this.loadData();
  }

  async loadData() {
    await this.loadSlice();
    await this.loadJankSlice();
    this.loaded = true;
  }

  async loadSlice() {
    this.sliceDetails =
        await getSlice(this.engine, asSliceSqlId(this.config.id));
    raf.scheduleRedraw();
  }

  async loadJankSlice() {
    if (exists(this.sliceDetails)) {
      // Get the id for the top-level EventLatency slice (this or parent), as
      // this id is used in the ScrollJankV3 track to identify the corresponding
      // janky interval.
      let eventLatencyId = -1;
      if (this.sliceDetails.name == 'EventLatency') {
        eventLatencyId = this.sliceDetails.id;
      } else {
        const queryResult = await this.engine.query(`
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
      }

      const possibleSlices =
          await getScrollJankSlices(this.engine, eventLatencyId);
      // We may not get any slices if the EventLatency doesn't indicate any
      // jank occurred.
      if (possibleSlices.length > 0) {
        this.jankySlice = possibleSlices[0];
      }
    }
  }

  private getLinksSection(): m.Child {
    return m(
        Section,
        {title: 'Quick links'},
        m(
            Tree,
            m(TreeNode, {
              left: exists(this.sliceDetails) ?
                  sliceRef(
                      this.sliceDetails,
                      'EventLatency in context of other Input events') :
                  'EventLatency in context of other Input events',
              right: exists(this.sliceDetails) ? '' : 'N/A',
            }),
            m(TreeNode, {
              left: exists(this.jankySlice) ? getSliceForTrack(
                                                  this.jankySlice,
                                                  ScrollJankV3Track.kind,
                                                  'Jank Interval') :
                                              'Jank Interval',
              right: exists(this.jankySlice) ? '' : 'N/A',
            }),
            ),
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
          text:
              `Note however the concept of coalescing or terminating early. This
               occurs when we receive multiple events or handle them quickly by
               converting them into a different event. Such as a TOUCH_MOVE
               being converted into a GESTURE_SCROLL_UPDATE type, or a multiple
               GESTURE_SCROLL_UPDATE events being formed into a single frame at
               the end of the RendererCompositorQueuingDelay.`,
        }),
        m(TextParagraph, {
          text:
              `*Important:* On some platforms (MacOS) we do not get feedback on
               when something is presented on the screen so the timings are only
               accurate for what we know on a given platform.`,
        }),
    );
  }

  viewTab() {
    if (exists(this.sliceDetails)) {
      const slice = this.sliceDetails;
      return m(
          DetailsShell,
          {
            title: 'Slice',
            description: slice.name,
          },
          m(GridLayout,
            m(GridLayoutColumn,
              renderDetails(slice),
              renderArguments(this.engine, slice)),
            m(GridLayoutColumn,
              m(Section,
                {title: 'Description'},
                m('.div', this.getDescriptionText())),
              this.getLinksSection())),
      );
    } else {
      return m(DetailsShell, {title: 'Slice', description: 'Loading...'});
    }
  }

  isLoading() {
    return !this.loaded;
  }

  getTitle(): string {
    return `Current Selection`;
  }
}

bottomTabRegistry.register(EventLatencySliceDetailsPanel);
