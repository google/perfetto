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
import {RecordPage} from './record_page';
import {RecordPageV2} from './record_page_v2';
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {RecordingPageController} from './recordingV2/recording_page_controller';
import {RecordingManager} from './recording_manager';
import {PageAttrs} from '../../public/page';
import {bindMithrilAttrs} from '../../base/mithril_utils';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.RecordTrace';

  static onActivate(app: App) {
    app.sidebar.addMenuItem({
      section: 'navigation',
      text: 'Record new trace',
      href: '#!/record',
      icon: 'fiber_smart_record',
      sortOrder: 2,
    });

    const RECORDING_V2_FLAG = app.featureFlags.register({
      id: 'recordingv2',
      name: 'Recording V2',
      description: 'Record using V2 interface',
      defaultValue: false,
    });
    const useRecordingV2 = RECORDING_V2_FLAG.get();

    const recMgr = new RecordingManager(app, useRecordingV2);
    let page: m.ClassComponent<PageAttrs>;
    if (useRecordingV2) {
      const recCtl = new RecordingPageController(app, recMgr);
      recCtl.initFactories();
      page = bindMithrilAttrs(RecordPageV2, {app, recCtl, recMgr});
    } else {
      page = bindMithrilAttrs(RecordPage, {app, recMgr});
    }
    app.pages.registerPage({route: '/record', traceless: true, page});
  }
}
