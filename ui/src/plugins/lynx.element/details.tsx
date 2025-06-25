// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import m from 'mithril';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {ElementDetailView} from './element_detail_panel';
import ElementManager from './element_manager';
import {LynxElement} from '../../lynx_perf/common_components/element_tree/types';

export class ElementDetailsPanel implements TrackEventDetailsPanel {
  private elementTreeDetails: LynxElement[] | undefined;
  private loading: boolean;

  constructor() {
    this.loading = false;
    this.elementTreeDetails = undefined;
  }

  async load({eventId}: TrackEventSelection) {
    this.loading = true;

    this.elementTreeDetails = ElementManager.getTraceIssueElements(eventId);

    this.loading = false;
  }

  render() {
    if (this.loading) {
      return m('h2', 'Loading');
    }

    if (this.elementTreeDetails) {
      return m(ElementDetailView, {details: this.elementTreeDetails});
    } else {
      return m('h2', 'No issue element nodes');
    }
  }
}
