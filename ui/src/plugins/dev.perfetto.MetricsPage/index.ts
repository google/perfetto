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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {MetricsPage} from './metrics_page';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.MetricsPage';

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.pages.registerPage({route: '/metrics', page: MetricsPage});
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Metrics',
      href: '#!/metrics',
      icon: 'speed',
      sortOrder: 9,
    });
  }
}
