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


import {AggregateData} from '../../common/aggregation_data';

import {Engine} from '../../common/engine';
import {TimestampedAreaSelection} from '../../common/state';

import {Controller} from '../controller';
import {globals} from '../globals';

export interface AggregationControllerArgs {
  engine: Engine;
  kind: string;
}

export abstract class AggregationController extends Controller<'main'> {
  private previousArea: TimestampedAreaSelection = {lastUpdate: 0};
  private requestingData = false;
  private queuedRequest = false;

  // Must be overridden by the aggregation implementation. It is invoked
  // whenever the selected area is changed and returns data to be published.
  abstract async onAreaSelectionChange(
      engine: Engine, area: TimestampedAreaSelection): Promise<AggregateData>;

  constructor(private args: AggregationControllerArgs) {
    super('main');
  }

  run() {
    const selectedArea = globals.state.frontendLocalState.selectedArea;
    if (this.previousArea &&
        this.previousArea.lastUpdate >= selectedArea.lastUpdate) {
      return;
    }
    if (this.requestingData) {
      this.queuedRequest = true;
    } else {
      this.requestingData = true;
      Object.assign(this.previousArea, selectedArea);
      this.onAreaSelectionChange(this.args.engine, selectedArea)
          .then(
              data => globals.publish(
                  'AggregateData', {data, kind: this.args.kind}))
          .catch(reason => {
            console.error(reason);
          })
          .finally(() => {
            this.requestingData = false;
            if (this.queuedRequest) {
              this.queuedRequest = false;
              this.run();
            }
          });
    }
  }
}