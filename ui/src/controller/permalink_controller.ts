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

import {produce} from 'immer';

import {assertExists, assertTrue} from '../base/logging';
import {Actions} from '../common/actions';
import {State} from '../common/state';
import {RecordConfig} from '../common/state';
import {
  BUCKET_NAME,
  saveState,
  saveTrace,
  toSha256
} from '../common/upload_utils';

import {Controller} from './controller';
import {globals} from './globals';
import {validateRecordConfig} from './validate_config';

export class PermalinkController extends Controller<'main'> {
  private lastRequestId?: string;
  constructor() {
    super('main');
  }

  run() {
    if (globals.state.permalink.requestId === undefined ||
        globals.state.permalink.requestId === this.lastRequestId) {
      return;
    }
    const requestId = assertExists(globals.state.permalink.requestId);
    this.lastRequestId = requestId;

    // if the |hash| is not set, this is a request to create a permalink.
    if (globals.state.permalink.hash === undefined) {
      const isRecordingConfig =
          assertExists(globals.state.permalink.isRecordingConfig);

      PermalinkController.createPermalink(isRecordingConfig)
          .then(((hash: string) => {
            globals.dispatch(Actions.setPermalink({requestId, hash}));
          }));
      return;
    }

    // Otherwise, this is a request to load the permalink.
    PermalinkController.loadState(globals.state.permalink.hash)
        .then(stateOrConfig => {
          if (this.isRecordConfig(stateOrConfig)) {
            // This permalink state only contains a RecordConfig. Show the
            // recording page with the config, but keep other state as-is.
            const validConfig = validateRecordConfig(stateOrConfig);
            if (validConfig.errorMessage) {
              // TODO(bsebastien): Show a warning message to the user in the UI.
              console.warn(validConfig.errorMessage);
            }
            globals.dispatch(
                Actions.setRecordConfig({config: validConfig.config}));
            globals.dispatch(Actions.navigate({route: '/record'}));
            return;
          }
          globals.dispatch(Actions.setState({newState: stateOrConfig}));
          this.lastRequestId = stateOrConfig.permalink.requestId;
        });
  }

  private isRecordConfig(stateOrConfig: State|
                         RecordConfig): stateOrConfig is RecordConfig {
    return ['STOP_WHEN_FULL', 'RING_BUFFER', 'LONG_TRACE'].includes(
        stateOrConfig.mode);
  }

  private static async createPermalink(isRecordingConfig: boolean) {
    let uploadState: State|RecordConfig = globals.state;

    if (isRecordingConfig) {
      uploadState = globals.state.recordConfig;
    } else {
      const engines = Object.values(globals.state.engines);
      assertTrue(engines.length === 1);
      const engine = engines[0];
      let dataToUpload: File|ArrayBuffer|undefined = undefined;
      let traceName = `trace ${engine.id}`;
      if (engine.source.type === 'FILE') {
        dataToUpload = engine.source.file;
        traceName = dataToUpload.name;
      } else if (engine.source.type === 'ARRAY_BUFFER') {
        dataToUpload = engine.source.buffer;
      } else if (engine.source.type !== 'URL') {
        throw new Error(`Cannot share trace ${JSON.stringify(engine.source)}`);
      }

      if (dataToUpload !== undefined) {
        PermalinkController.updateStatus(`Uploading ${traceName}`);
        const url = await saveTrace(dataToUpload);
        // Convert state to use URLs and remove permalink.
        uploadState = produce(globals.state, draft => {
          draft.engines[engine.id].source = {type: 'URL', url};
          draft.permalink = {};
        });
      }
    }

    // Upload state.
    PermalinkController.updateStatus(`Creating permalink...`);
    const hash = await saveState(uploadState);
    PermalinkController.updateStatus(`Permalink ready`);
    return hash;
  }

  private static async loadState(id: string): Promise<State|RecordConfig> {
    const url = `https://storage.googleapis.com/${BUCKET_NAME}/${id}`;
    const response = await fetch(url);
    if (!response.ok) {
      throw new Error(
          `Could not fetch permalink.\n` +
          `Are you sure the id (${id}) is correct?\n` +
          `URL: ${url}`);
    }
    const text = await response.text();
    const stateHash = await toSha256(text);
    const state = JSON.parse(text);
    if (stateHash !== id) {
      throw new Error(`State hash does not match ${id} vs. ${stateHash}`);
    }
    return state;
  }

  private static updateStatus(msg: string): void {
    // TODO(hjd): Unify loading updates.
    globals.dispatch(Actions.updateStatus({
      msg,
      timestamp: Date.now() / 1000,
    }));
  }
}
