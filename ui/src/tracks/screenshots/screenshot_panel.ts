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

import {assertTrue} from '../../base/logging';
import {exists} from '../../base/utils';
import {EngineProxy} from '../../common/engine';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {getSlice, SliceDetails} from '../../frontend/sql/slice';
import {asSliceSqlId} from '../../frontend/sql_types';

async function getSliceDetails(
    engine: EngineProxy, id: number): Promise<SliceDetails|undefined> {
  return getSlice(engine, asSliceSqlId(id));
}

export class ScreenshotTab extends BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'dev.perfetto.ScreenshotDetailsPanel';

  private sliceDetails?: SliceDetails;

  static create(args: NewBottomTabArgs): ScreenshotTab {
    return new ScreenshotTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
    getSliceDetails(this.engine, this.config.id)
        .then((sliceDetails) => this.sliceDetails = sliceDetails);
  }

  renderTabCanvas() {}

  getTitle() {
    return this.config.title;
  }

  viewTab() {
    if (!exists(this.sliceDetails) || !exists(this.sliceDetails.args) ||
        this.sliceDetails.args.length == 0) {
      return m('h2', 'Loading Screenshot');
    }
    assertTrue(this.sliceDetails.args[0].key == 'screenshot.jpg_image');
    return m('.screenshot-panel', m('img', {
               src: 'data:image/png;base64, ' +
                   this.sliceDetails.args[0].displayValue,
             }));
  }
}

bottomTabRegistry.register(ScreenshotTab);
