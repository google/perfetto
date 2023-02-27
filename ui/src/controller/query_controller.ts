// Copyright (C) 2018 The Android Open Source Project
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

import {assertExists} from '../base/logging';
import {Actions} from '../common/actions';
import {Engine} from '../common/engine';
import {runQuery} from '../common/queries';
import {globals} from '../frontend/globals';
import {publishQueryResult} from '../frontend/publish';

import {Controller} from './controller';

export interface QueryControllerArgs {
  queryId: string;
  engine: Engine;
}

export class QueryController extends Controller<'init'|'querying'> {
  constructor(private args: QueryControllerArgs) {
    super('init');
  }

  run() {
    switch (this.state) {
      case 'init':
        const config = assertExists(globals.state.queries[this.args.queryId]);
        runQuery(this.args.queryId, config.query, this.args.engine)
            .then((result) => {
              console.log(`Query ${config.query} took ${result.durationMs} ms`);
              publishQueryResult({id: this.args.queryId, data: result});
              globals.dispatch(
                  Actions.deleteQuery({queryId: this.args.queryId}));
            });
        this.setState('querying');
        break;

      case 'querying':
        // Nothing to do here, as soon as the deleteQuery is dispatched this
        // controller will be destroyed (by the TraceController).
        break;

      default:
        throw new Error(`Unexpected state ${this.state}`);
    }
  }
}
