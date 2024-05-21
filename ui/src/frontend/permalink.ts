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

import {produce} from 'immer';
import {assertExists} from '../base/logging';
import {runValidator} from '../base/validators';
import {Actions} from '../common/actions';
import {ConversionJobStatus} from '../common/conversion_jobs';
import {
  createEmptyNonSerializableState,
  createEmptyState,
} from '../common/empty_state';
import {EngineConfig, ObjectById, STATE_VERSION, State} from '../common/state';
import {
  BUCKET_NAME,
  TraceGcsUploader,
  buggyToSha256,
  deserializeStateObject,
  saveState,
  toSha256,
} from '../common/upload_utils';
import {
  RecordConfig,
  recordConfigValidator,
} from '../controller/record_config_types';
import {globals} from './globals';
import {
  publishConversionJobStatusUpdate,
  publishPermalinkHash,
} from './publish';
import {Router} from './router';
import {showModal} from '../widgets/modal';

export interface PermalinkOptions {
  isRecordingConfig?: boolean;
}

export async function createPermalink(
  options: PermalinkOptions = {},
): Promise<void> {
  const {isRecordingConfig = false} = options;
  const jobName = 'create_permalink';
  publishConversionJobStatusUpdate({
    jobName,
    jobStatus: ConversionJobStatus.InProgress,
  });

  try {
    const hash = await createPermalinkInternal(isRecordingConfig);
    publishPermalinkHash(hash);
  } finally {
    publishConversionJobStatusUpdate({
      jobName,
      jobStatus: ConversionJobStatus.NotRunning,
    });
  }
}

async function createPermalinkInternal(
  isRecordingConfig: boolean,
): Promise<string> {
  let uploadState: State | RecordConfig = globals.state;

  if (isRecordingConfig) {
    uploadState = globals.state.recordConfig;
  } else {
    const engine = assertExists(globals.getCurrentEngine());
    let dataToUpload: File | ArrayBuffer | undefined = undefined;
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
      updateStatus(`Uploading ${traceName}`);
      const uploader = new TraceGcsUploader(dataToUpload, () => {
        switch (uploader.state) {
          case 'UPLOADING':
            const statusTxt = `Uploading ${uploader.getEtaString()}`;
            updateStatus(statusTxt);
            break;
          case 'UPLOADED':
            // Convert state to use URLs and remove permalink.
            const url = uploader.uploadedUrl;
            uploadState = produce(globals.state, (draft) => {
              assertExists(draft.engine).source = {type: 'URL', url};
            });
            break;
          case 'ERROR':
            updateStatus(`Upload failed ${uploader.error}`);
            break;
        } // switch (state)
      }); // onProgress
      await uploader.waitForCompletion();
    }
  }

  // Upload state.
  updateStatus(`Creating permalink...`);
  const hash = await saveState(uploadState);
  updateStatus(`Permalink ready`);
  return hash;
}

function updateStatus(msg: string): void {
  // TODO(hjd): Unify loading updates.
  globals.dispatch(
    Actions.updateStatus({
      msg,
      timestamp: Date.now() / 1000,
    }),
  );
}

export async function loadPermalink(hash: string): Promise<void> {
  // Otherwise, this is a request to load the permalink.
  const stateOrConfig = await loadState(hash);

  if (isRecordConfig(stateOrConfig)) {
    // This permalink state only contains a RecordConfig. Show the
    // recording page with the config, but keep other state as-is.
    const validConfig = runValidator(
      recordConfigValidator,
      stateOrConfig as unknown,
    ).result;
    globals.dispatch(Actions.setRecordConfig({config: validConfig}));
    Router.navigate('#!/record');
    return;
  }
  globals.dispatch(Actions.setState({newState: stateOrConfig}));
}

async function loadState(id: string): Promise<State | RecordConfig> {
  const url = `https://storage.googleapis.com/${BUCKET_NAME}/${id}`;
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(
      `Could not fetch permalink.\n` +
        `Are you sure the id (${id}) is correct?\n` +
        `URL: ${url}`,
    );
  }
  const text = await response.text();
  const stateHash = await toSha256(text);
  const state = deserializeStateObject<State>(text);
  if (stateHash !== id) {
    // Old permalinks incorrectly dropped some digits from the
    // hexdigest of the SHA256. We don't want to invalidate those
    // links so we also compute the old string and try that here
    // also.
    const buggyStateHash = await buggyToSha256(text);
    if (buggyStateHash !== id) {
      throw new Error(`State hash does not match ${id} vs. ${stateHash}`);
    }
  }
  if (!isRecordConfig(state)) {
    return upgradeState(state);
  }
  return state;
}

function isRecordConfig(
  stateOrConfig: State | RecordConfig,
): stateOrConfig is RecordConfig {
  const mode = (stateOrConfig as {mode?: string}).mode;
  return (
    mode !== undefined &&
    ['STOP_WHEN_FULL', 'RING_BUFFER', 'LONG_TRACE'].includes(mode)
  );
}

function upgradeState(state: State): State {
  if (state.engine !== undefined && state.engine.source.type !== 'URL') {
    // All permalink traces should be modified to have a source.type=URL
    // pointing to the uploaded trace. Due to a bug in some older version
    // of the UI (b/327049372), an upload failure can end up with a state that
    // has type=FILE but a null file object. If this happens, invalidate the
    // trace and show a message.
    showModal({
      title: 'Cannot load trace permalink',
      content: m(
        'div',
        'The permalink stored on the server is corrupted ' +
          'and cannot be loaded.',
      ),
    });
    return createEmptyState();
  }

  if (state.version !== STATE_VERSION) {
    const newState = createEmptyState();
    // Old permalinks from state versions prior to version 24
    // have multiple engines of which only one is identified as the
    // current engine via currentEngineId. Handle this case:
    if (isMultiEngineState(state)) {
      const engineId = state.currentEngineId;
      if (engineId !== undefined) {
        newState.engine = state.engines[engineId];
      }
    } else {
      newState.engine = state.engine;
    }

    if (newState.engine !== undefined) {
      newState.engine.ready = false;
    }
    const message =
      `Unable to parse old state version. Discarding state ` +
      `and loading trace.`;
    console.warn(message);
    updateStatus(message);
    return newState;
  } else {
    // Loaded state is presumed to be compatible with the State type
    // definition in the app. However, a non-serializable part has to be
    // recreated.
    state.nonSerializableState = createEmptyNonSerializableState();
  }
  return state;
}

interface MultiEngineState {
  currentEngineId?: string;
  engines: ObjectById<EngineConfig>;
}

function isMultiEngineState(
  state: State | MultiEngineState,
): state is MultiEngineState {
  if ((state as MultiEngineState).engines !== undefined) {
    return true;
  }
  return false;
}
