// Copyright (C) 2022 The Android Open Source Project
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

import {assertExists, assertTrue} from '../../base/logging';
import {currentDateHourAndMinute} from '../../base/time';
import {raf} from '../../core/raf_scheduler';
import {globals} from '../../frontend/globals';
import {autosaveConfigStore} from '../../frontend/record_config';
import {
  DEFAULT_ADB_WEBSOCKET_URL,
  DEFAULT_TRACED_WEBSOCKET_URL,
} from '../../frontend/recording/recording_ui_utils';
import {
  couldNotClaimInterface,
} from '../../frontend/recording/reset_interface_modal';
import {TraceConfig} from '../../protos';
import {Actions} from '../actions';
import {TRACE_SUFFIX} from '../constants';

import {genTraceConfig} from './recording_config_utils';
import {RecordingError, showRecordingModal} from './recording_error_handling';
import {
  RecordingTargetV2,
  TargetInfo,
  TracingSession,
  TracingSessionListener,
} from './recording_interfaces_v2';
import {
  BUFFER_USAGE_NOT_ACCESSIBLE,
  RECORDING_IN_PROGRESS,
} from './recording_utils';
import {
  ANDROID_WEBSOCKET_TARGET_FACTORY,
  AndroidWebsocketTargetFactory,
} from './target_factories/android_websocket_target_factory';
import {
  ANDROID_WEBUSB_TARGET_FACTORY,
} from './target_factories/android_webusb_target_factory';
import {
  HOST_OS_TARGET_FACTORY,
  HostOsTargetFactory,
} from './target_factories/host_os_target_factory';
import {targetFactoryRegistry} from './target_factory_registry';

// The recording page can be in any of these states. It can transition between
// states:
// a) because of a user actions - pressing a UI button ('Start', 'Stop',
//    'Cancel', 'Force reset' of the target), selecting a different target in
//    the UI, authorizing authentication on an Android device,
//    pulling the cable which connects an Android device.
// b) automatically - if there is no need to reset the device or if the user
//    has previously authorised the device to be debugged via USB.
//
// Recording state machine: https://screenshot.googleplex.com/BaX5EGqQMajgV7G
export enum RecordingState {
  NO_TARGET = 0,
  TARGET_SELECTED = 1,
  // P1 stands for 'Part 1', where we first connect to the device in order to
  // obtain target information.
  ASK_TO_FORCE_P1 = 2,
  AUTH_P1 = 3,
  TARGET_INFO_DISPLAYED = 4,
  // P2 stands for 'Part 2', where we connect to device for the 2nd+ times, to
  // record a tracing session.
  ASK_TO_FORCE_P2 = 5,
  AUTH_P2 = 6,
  RECORDING = 7,
  WAITING_FOR_TRACE_DISPLAY = 8,
}

// Wraps a tracing session promise while the promise is being resolved (e.g.
// while we are awaiting for ADB auth).
class TracingSessionWrapper {
  private tracingSession?: TracingSession = undefined;
  private isCancelled = false;
  // We only execute the logic in the callbacks if this TracingSessionWrapper
  // is the one referenced by the controller. Otherwise this can hold a
  // tracing session which the user has already cancelled, so it shouldn't
  // influence the UI.
  private tracingSessionListener: TracingSessionListener = {
    onTraceData: (trace: Uint8Array) =>
        this.controller.maybeOnTraceData(this, trace),
    onStatus: (message) => this.controller.maybeOnStatus(this, message),
    onDisconnect: (errorMessage?: string) =>
        this.controller.maybeOnDisconnect(this, errorMessage),
    onError: (errorMessage: string) =>
        this.controller.maybeOnError(this, errorMessage),
  };

  private target: RecordingTargetV2;
  private controller: RecordingPageController;

  constructor(target: RecordingTargetV2, controller: RecordingPageController) {
    this.target = target;
    this.controller = controller;
  }

  async start(traceConfig: TraceConfig) {
    let stateGeneratioNr = this.controller.getStateGeneration();
    const createSession = async () => {
      try {
        this.controller.maybeSetState(
            this, RecordingState.AUTH_P2, stateGeneratioNr);
        stateGeneratioNr += 1;

        const session =
            await this.target.createTracingSession(this.tracingSessionListener);

        // We check the `isCancelled` to see if the user has cancelled the
        // tracing session before it becomes available in TracingSessionWrapper.
        if (this.isCancelled) {
          session.cancel();
          return;
        }

        this.tracingSession = session;
        this.controller.maybeSetState(
            this, RecordingState.RECORDING, stateGeneratioNr);
        // When the session is resolved, the traceConfig has been instantiated.
        this.tracingSession.start(assertExists(traceConfig));
      } catch (e) {
        this.tracingSessionListener.onError(e.message);
      }
    };

    if (await this.target.canConnectWithoutContention()) {
      await createSession();
    } else {
      // If we need to reset the connection to be able to connect, we ask
      // the user if they want to reset the connection.
      this.controller.maybeSetState(
          this, RecordingState.ASK_TO_FORCE_P2, stateGeneratioNr);
      stateGeneratioNr += 1;
      couldNotClaimInterface(
          createSession, () => this.controller.maybeClearRecordingState(this));
    }
  }

  async fetchTargetInfo() {
    let stateGeneratioNr = this.controller.getStateGeneration();
    const createSession = async () => {
      try {
        this.controller.maybeSetState(
            this, RecordingState.AUTH_P1, stateGeneratioNr);
        stateGeneratioNr += 1;
        await this.target.fetchTargetInfo(this.tracingSessionListener);
        this.controller.maybeSetState(
            this, RecordingState.TARGET_INFO_DISPLAYED, stateGeneratioNr);
      } catch (e) {
        this.tracingSessionListener.onError(e.message);
      }
    };

    if (await this.target.canConnectWithoutContention()) {
      await createSession();
    } else {
      // If we need to reset the connection to be able to connect, we ask
      // the user if they want to reset the connection.
      this.controller.maybeSetState(
          this, RecordingState.ASK_TO_FORCE_P1, stateGeneratioNr);
      stateGeneratioNr += 1;
      couldNotClaimInterface(
          createSession,
          () => this.controller.maybeSetState(
              this, RecordingState.TARGET_SELECTED, stateGeneratioNr));
    }
  }

  cancel() {
    if (this.tracingSession) {
      this.tracingSession.cancel();
    } else {
      // In some cases, the tracingSession may not be available to the
      // TracingSessionWrapper when the user cancels it.
      // For instance:
      //  1. The user clicked 'Start'.
      //  2. They clicked 'Stop' without authorizing on the device.
      //  3. They clicked 'Start'.
      //  4. They authorized on the device.
      // In these cases, we want to cancel the tracing session as soon as it
      // becomes available. Therefore, we keep the `isCancelled` boolean and
      // check it when we receive the tracing session.
      this.isCancelled = true;
    }
    this.controller.maybeClearRecordingState(this);
  }

  stop() {
    const stateGeneratioNr = this.controller.getStateGeneration();
    if (this.tracingSession) {
      this.tracingSession.stop();
      this.controller.maybeSetState(
          this, RecordingState.WAITING_FOR_TRACE_DISPLAY, stateGeneratioNr);
    } else {
      // In some cases, the tracingSession may not be available to the
      // TracingSessionWrapper when the user stops it.
      // For instance:
      //  1. The user clicked 'Start'.
      //  2. They clicked 'Stop' without authorizing on the device.
      //  3. They clicked 'Start'.
      //  4. They authorized on the device.
      // In these cases, we want to cancel the tracing session as soon as it
      // becomes available. Therefore, we keep the `isCancelled` boolean and
      // check it when we receive the tracing session.
      this.isCancelled = true;
      this.controller.maybeClearRecordingState(this);
    }
  }

  getTraceBufferUsage(): Promise<number> {
    if (!this.tracingSession) {
      throw new RecordingError(BUFFER_USAGE_NOT_ACCESSIBLE);
    }
    return this.tracingSession.getTraceBufferUsage();
  }
}

// Keeps track of the state the Ui is in. Has methods which are executed on
// user actions such as starting/stopping/cancelling a tracing session.
export class RecordingPageController {
  // State of the recording page. This is set by user actions and/or automatic
  // transitions. This is queried by the UI in order to
  private state: RecordingState = RecordingState.NO_TARGET;
  // Currently selected target.
  private target?: RecordingTargetV2 = undefined;
  // We wrap the tracing session in an object, because for some targets
  // (Ex: Android) it is only created after we have succesfully authenticated
  // with the target.
  private tracingSessionWrapper?: TracingSessionWrapper = undefined;
  // How much of the buffer is used for the current tracing session.
  private bufferUsagePercentage: number = 0;
  // A counter for state modifications. We use this to ensure that state
  // transitions don't override one another in async functions.
  private stateGeneration = 0;

  getBufferUsagePercentage(): number {
    return this.bufferUsagePercentage;
  }

  getState(): RecordingState {
    return this.state;
  }

  getStateGeneration(): number {
    return this.stateGeneration;
  }

  maybeSetState(
      tracingSessionWrapper: TracingSessionWrapper, state: RecordingState,
      stateGeneration: number): void {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    if (stateGeneration !== this.stateGeneration) {
      throw new RecordingError('Recording page state transition out of order.');
    }
    this.setState(state);
    globals.dispatch(Actions.setRecordingStatus({status: undefined}));
    raf.scheduleFullRedraw();
  }

  maybeClearRecordingState(tracingSessionWrapper: TracingSessionWrapper): void {
    if (this.tracingSessionWrapper === tracingSessionWrapper) {
      this.clearRecordingState();
    }
  }

  maybeOnTraceData(
      tracingSessionWrapper: TracingSessionWrapper, trace: Uint8Array) {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    globals.dispatch(Actions.openTraceFromBuffer({
      title: 'Recorded trace',
      buffer: trace.buffer,
      fileName: `trace_${currentDateHourAndMinute()}${TRACE_SUFFIX}`,
    }));
    this.clearRecordingState();
  }

  maybeOnStatus(tracingSessionWrapper: TracingSessionWrapper, message: string) {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    // For the 'Recording in progress for 7000ms we don't show a
    // modal.'
    if (message.startsWith(RECORDING_IN_PROGRESS)) {
      globals.dispatch(Actions.setRecordingStatus({status: message}));
    } else {
      // For messages such as 'Please allow USB debugging on your
      // device, which require a user action, we show a modal.
      showRecordingModal(message);
    }
  }

  maybeOnDisconnect(
      tracingSessionWrapper: TracingSessionWrapper, errorMessage?: string) {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    if (errorMessage) {
      showRecordingModal(errorMessage);
    }
    this.clearRecordingState();
    this.onTargetChange();
  }

  maybeOnError(
      tracingSessionWrapper: TracingSessionWrapper, errorMessage: string) {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    showRecordingModal(errorMessage);
    this.clearRecordingState();
  }

  getTargetInfo(): TargetInfo|undefined {
    if (!this.target) {
      return undefined;
    }
    return this.target.getInfo();
  }

  canCreateTracingSession() {
    if (!this.target) {
      return false;
    }
    return this.target.canCreateTracingSession();
  }

  selectTarget(selectedTarget?: RecordingTargetV2) {
    assertTrue(
        RecordingState.NO_TARGET <= this.state &&
        this.state < RecordingState.RECORDING);
    // If the selected target exists and is the same as the previous one, we
    // don't need to do anything.
    if (selectedTarget && selectedTarget === this.target) {
      return;
    }

    // We assign the new target and redraw the page.
    this.target = selectedTarget;

    if (!this.target) {
      this.setState(RecordingState.NO_TARGET);
      raf.scheduleFullRedraw();
      return;
    }
    this.setState(RecordingState.TARGET_SELECTED);
    raf.scheduleFullRedraw();

    this.tracingSessionWrapper = this.createTracingSessionWrapper(this.target);
    this.tracingSessionWrapper.fetchTargetInfo();
  }

  async addAndroidDevice(): Promise<void> {
    try {
      const target =
          await targetFactoryRegistry.get(ANDROID_WEBUSB_TARGET_FACTORY)
              .connectNewTarget();
      this.selectTarget(target);
    } catch (e) {
      if (e instanceof RecordingError) {
        showRecordingModal(e.message);
      } else {
        throw e;
      }
    }
  }

  onTargetSelection(targetName: string): void {
    assertTrue(
        RecordingState.NO_TARGET <= this.state &&
        this.state < RecordingState.RECORDING);
    const allTargets = targetFactoryRegistry.listTargets();
    this.selectTarget(allTargets.find((t) => t.getInfo().name === targetName));
  }

  onStartRecordingPressed(): void {
    assertTrue(RecordingState.TARGET_INFO_DISPLAYED === this.state);
    location.href = '#!/record/instructions';
    autosaveConfigStore.save(globals.state.recordConfig);

    const target = this.getTarget();
    const targetInfo = target.getInfo();
    globals.logging.logEvent(
        'Record Trace', `Record trace (${targetInfo.targetType})`);
    const traceConfig = genTraceConfig(globals.state.recordConfig, targetInfo);

    this.tracingSessionWrapper = this.createTracingSessionWrapper(target);
    this.tracingSessionWrapper.start(traceConfig);
  }

  onCancel() {
    assertTrue(
        RecordingState.AUTH_P2 <= this.state &&
        this.state <= RecordingState.RECORDING);
    // The 'Cancel' button will only be shown after a `tracingSessionWrapper`
    // is created.
    this.getTracingSessionWrapper().cancel();
  }

  onStop() {
    assertTrue(
        RecordingState.AUTH_P2 <= this.state &&
        this.state <= RecordingState.RECORDING);
    // The 'Stop' button will only be shown after a `tracingSessionWrapper`
    // is created.
    this.getTracingSessionWrapper().stop();
  }

  async fetchBufferUsage() {
    assertTrue(this.state >= RecordingState.AUTH_P2);
    if (!this.tracingSessionWrapper) return;
    const session = this.tracingSessionWrapper;

    try {
      const usage = await session.getTraceBufferUsage();
      if (this.tracingSessionWrapper === session) {
        this.bufferUsagePercentage = usage;
      }
    } catch (e) {
      // We ignore RecordingErrors because they are not necessary for the trace
      // to be successfully collected.
      if (!(e instanceof RecordingError)) {
        throw e;
      }
    }
    // We redraw if:
    // 1. We received a correct buffer usage value.
    // 2. We receive a RecordingError.
    raf.scheduleFullRedraw();
  }

  initFactories() {
    assertTrue(this.state <= RecordingState.TARGET_INFO_DISPLAYED);
    for (const targetFactory of targetFactoryRegistry.listTargetFactories()) {
      if (targetFactory) {
        targetFactory.setOnTargetChange(this.onTargetChange.bind(this));
      }
    }

    if (targetFactoryRegistry.has(ANDROID_WEBSOCKET_TARGET_FACTORY)) {
      const websocketTargetFactory =
          targetFactoryRegistry.get(ANDROID_WEBSOCKET_TARGET_FACTORY) as
          AndroidWebsocketTargetFactory;
      websocketTargetFactory.tryEstablishWebsocket(DEFAULT_ADB_WEBSOCKET_URL);
    }
    if (targetFactoryRegistry.has(HOST_OS_TARGET_FACTORY)) {
      const websocketTargetFactory =
          targetFactoryRegistry.get(HOST_OS_TARGET_FACTORY) as
          HostOsTargetFactory;
      websocketTargetFactory.tryEstablishWebsocket(
          DEFAULT_TRACED_WEBSOCKET_URL);
    }
  }

  shouldShowTargetSelection(): boolean {
    return RecordingState.NO_TARGET <= this.state &&
        this.state < RecordingState.RECORDING;
  }

  shouldShowStopCancelButtons(): boolean {
    return RecordingState.AUTH_P2 <= this.state &&
        this.state <= RecordingState.RECORDING;
  }

  private onTargetChange() {
    const allTargets = targetFactoryRegistry.listTargets();
    // If the change happens for an existing target, the controller keeps the
    // currently selected target in focus.
    if (this.target && allTargets.includes(this.target)) {
      raf.scheduleFullRedraw();
      return;
    }
    // If the change happens to a new target or the controller does not have a
    // defined target, the selection process again is run again.
    this.selectTarget();
  }

  private createTracingSessionWrapper(target: RecordingTargetV2):
      TracingSessionWrapper {
    return new TracingSessionWrapper(target, this);
  }

  private clearRecordingState(): void {
    this.bufferUsagePercentage = 0;
    this.tracingSessionWrapper = undefined;
    this.setState(RecordingState.TARGET_INFO_DISPLAYED);
    globals.dispatch(Actions.setRecordingStatus({status: undefined}));
    // Redrawing because this method has changed the RecordingState, which will
    // affect the display of the record_page.
    raf.scheduleFullRedraw();
  }

  private setState(state: RecordingState) {
    this.state = state;
    this.stateGeneration += 1;
  }

  private getTarget(): RecordingTargetV2 {
    assertTrue(RecordingState.TARGET_INFO_DISPLAYED === this.state);
    return assertExists(this.target);
  }

  private getTracingSessionWrapper(): TracingSessionWrapper {
    assertTrue(
        RecordingState.ASK_TO_FORCE_P2 <= this.state &&
        this.state <= RecordingState.RECORDING);
    return assertExists(this.tracingSessionWrapper);
  }
}
