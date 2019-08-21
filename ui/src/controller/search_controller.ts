// Copyright (C) 2019 The Android Open Source Project
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

import {Engine} from '../common/engine';
import {TimeSpan} from '../common/time';
import {Controller} from './controller';
import {App} from './globals';

export interface SearchControllerArgs {
  engine: Engine;
  app: App;
}

export class SearchController extends Controller<'main'> {
  private engine: Engine;
  private app: App;
  private previousSpan: TimeSpan;
  private queryInProgress: boolean;

  constructor(args: SearchControllerArgs) {
    super('main');
    this.engine = args.engine;
    this.app = args.app;
    this.previousSpan = new TimeSpan(0, 1);
    this.queryInProgress = false;
  }

  run() {
    const visibleState = this.app.state.frontendLocalState.visibleState;
    if (visibleState === undefined) {
      return;
    }
    const newSpan = new TimeSpan(visibleState.startSec, visibleState.endSec);
    if (this.previousSpan.equals(newSpan)) {
      return;
    }
    this.previousSpan = newSpan;

    if (this.queryInProgress) {
      return;
    }

    const startNs = Math.round(newSpan.start * 1e9);
    const endNs = Math.round(newSpan.end * 1e9);
    this.queryInProgress = true;
    this.engine
        .queryOneRow(`
        select count(*) from sched
        where ts > ${startNs} and ts < ${endNs};`)
        .then(row => {
          this.app.publish('Search', {
            resultCount: row[0],
          });
        })
        .finally(() => {
          this.queryInProgress = false;
          this.run();
        });
  }
}
