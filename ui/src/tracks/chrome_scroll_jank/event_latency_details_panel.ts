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
import {DetailsShell} from '../../frontend/widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../frontend/widgets/grid_layout';
import {Section} from '../../frontend/widgets/section';
import {Tree, TreeNode} from '../../frontend/widgets/tree';

export class EventLatencySliceDetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'dev.perfetto.EventLatencySliceDetailsPanel';

  private sliceDetails?: SliceDetails;

  static create(args: NewBottomTabArgs): EventLatencySliceDetailsPanel {
    return new EventLatencySliceDetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    // Start loading the slice details
    this.loadSlice();
  }

  async loadSlice() {
    this.sliceDetails =
        await getSlice(this.engine, asSliceSqlId(this.config.id));
    raf.scheduleRedraw();
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
            m(
                GridLayoutColumn,
                renderDetails(slice),
                ),
            m(GridLayoutColumn,
              renderArguments(this.engine, slice),
              m(
                  Section,
                  {title: 'Quick links'},
                  // TODO(hbolaria): add a link to the jank interval if this
                  // slice is a janky latency.
                  m(
                      Tree,
                      m(TreeNode, {
                        left: sliceRef(
                            this.sliceDetails, 'Original EventLatency'),
                        right: '',
                      }),
                      ),
                  ))),
      );
    } else {
      return m(DetailsShell, {title: 'Slice', description: 'Loading...'});
    }
  }

  isLoading() {
    return !exists(this.sliceDetails);
  }

  getTitle(): string {
    return `Current Selection`;
  }

  renderTabCanvas() {
    return;
  }
}

bottomTabRegistry.register(EventLatencySliceDetailsPanel);
