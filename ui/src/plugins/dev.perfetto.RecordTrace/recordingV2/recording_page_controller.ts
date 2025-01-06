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

import {assertExists, assertTrue} from '../../../base/logging';
import {currentDateHourAndMinute} from '../../../base/time';
import {RecordingManager} from '../recording_manager';
import {couldNotClaimInterface} from '../reset_interface_modal';
import {TraceConfig} from '../protos';
import {TRACE_SUFFIX} from '../../../public/trace';
import {RecordingError, showRecordingModal} from './recording_error_handling';
import {
  RecordingTargetV2,
  TargetInfo,
  TracingSession,
  TracingSessionListener,
} from './recording_interfaces_v2';
import {RECORDING_IN_PROGRESS} from './recording_utils';
import {targetFactoryRegistry} from './target_factory_registry';
import {App} from '../../../public/app';

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
          this,
          RecordingState.AUTH_P2,
          stateGeneratioNr,
        );
        stateGeneratioNr += 1;

        const session = await this.target.createTracingSession(
          this.tracingSessionListener,
        );

        // We check the `isCancelled` to see if the user has cancelled the
        // tracing session before it becomes available in TracingSessionWrapper.
        if (this.isCancelled) {
          session.cancel();
          return;
        }

        this.tracingSession = session;
        this.controller.maybeSetState(
          this,
          RecordingState.RECORDING,
          stateGeneratioNr,
        );
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
        this,
        RecordingState.ASK_TO_FORCE_P2,
        stateGeneratioNr,
      );
      stateGeneratioNr += 1;
      couldNotClaimInterface(createSession, () =>
        this.controller.maybeClearRecordingState(this),
      );
    }
  }

  async fetchTargetInfo() {
    let stateGeneratioNr = this.controller.getStateGeneration();
    const createSession = async () => {
      try {
        this.controller.maybeSetState(
          this,
          RecordingState.AUTH_P1,
          stateGeneratioNr,
        );
        stateGeneratioNr += 1;
        await this.target.fetchTargetInfo(this.tracingSessionListener);
        this.controller.maybeSetState(
          this,
          RecordingState.TARGET_INFO_DISPLAYED,
          stateGeneratioNr,
        );
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
        this,
        RecordingState.ASK_TO_FORCE_P1,
        stateGeneratioNr,
      );
      stateGeneratioNr += 1;
      couldNotClaimInterface(createSession, () =>
        this.controller.maybeSetState(
          this,
          RecordingState.TARGET_SELECTED,
          stateGeneratioNr,
        ),
      );
    }
  }
}

// Keeps track of the state the Ui is in. Has methods which are executed on
// user actions such as starting/stopping/cancelling a tracing session.
export class RecordingPageController {
  private app: App;
  private recMgr: RecordingManager;

  // State of the recording page. This is set by user actions and/or automatic
  // transitions. This is queried by the UI in order to
  private state: RecordingState = RecordingState.NO_TARGET;
  // Currently selected target.
  private target?: RecordingTargetV2 = undefined;
  // We wrap the tracing session in an object, because for some targets
  // (Ex: Android) it is only created after we have succesfully authenticated
  // with the target.
  private tracingSessionWrapper?: TracingSessionWrapper = undefined;
  // A counter for state modifications. We use this to ensure that state
  // transitions don't override one another in async functions.
  private stateGeneration = 0;

  constructor(app: App, recMgr: RecordingManager) {
    this.app = app;
    this.recMgr = recMgr;
  }

  getStateGeneration(): number {
    return this.stateGeneration;
  }

  maybeSetState(
    tracingSessionWrapper: TracingSessionWrapper,
    state: RecordingState,
    stateGeneration: number,
  ): void {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    if (stateGeneration !== this.stateGeneration) {
      throw new RecordingError('Recording page state transition out of order.');
    }
    this.setState(state);
    this.recMgr.setRecordingStatus(undefined);
    this.app.raf.scheduleFullRedraw();
  }

  maybeClearRecordingState(tracingSessionWrapper: TracingSessionWrapper): void {
    if (this.tracingSessionWrapper === tracingSessionWrapper) {
      this.clearRecordingState();
    }
  }

  maybeOnTraceData(
    tracingSessionWrapper: TracingSessionWrapper,
    trace: Uint8Array,
  ) {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    this.app.openTraceFromBuffer({
      title: 'Recorded trace',
      buffer: trace.buffer,
      fileName: `trace_${currentDateHourAndMinute()}${TRACE_SUFFIX}`,
    });
    this.clearRecordingState();
  }

  maybeOnStatus(tracingSessionWrapper: TracingSessionWrapper, message: string) {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    // For the 'Recording in progress for 7000ms we don't show a
    // modal.'
    if (message.startsWith(RECORDING_IN_PROGRESS)) {
      this.recMgr.setRecordingStatus(message);
    } else {
      // For messages such as 'Please allow USB debugging on your
      // device, which require a user action, we show a modal.
      showRecordingModal(message);
    }
  }

  maybeOnDisconnect(
    tracingSessionWrapper: TracingSessionWrapper,
    errorMessage?: string,
  ) {
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
    tracingSessionWrapper: TracingSessionWrapper,
    errorMessage: string,
  ) {
    if (this.tracingSessionWrapper !== tracingSessionWrapper) {
      return;
    }
    showRecordingModal(errorMessage);
    this.clearRecordingState();
  }

  getTargetInfo(): TargetInfo | undefined {
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
        this.state < RecordingState.RECORDING,
    );
    // If the selected target exists and is the same as the previous one, we
    // don't need to do anything.
    if (selectedTarget && selectedTarget === this.target) {
      return;
    }

    // We assign the new target and redraw the page.
    this.target = selectedTarget;

    if (!this.target) {
      this.setState(RecordingState.NO_TARGET);
      this.app.raf.scheduleFullRedraw();
      return;
    }
    this.setState(RecordingState.TARGET_SELECTED);
    this.app.raf.scheduleFullRedraw();

    this.tracingSessionWrapper = this.createTracingSessionWrapper(this.target);
    this.tracingSessionWrapper.fetchTargetInfo();
  }

  private onTargetChange() {
    const allTargets = targetFactoryRegistry.listTargets();
    // If the change happens for an existing target, the controller keeps the
    // currently selected target in focus.
    if (this.target && allTargets.includes(this.target)) {
      this.app.raf.scheduleFullRedraw();
      return;
    }
    // If the change happens to a new target or the controller does not have a
    // defined target, the selection process again is run again.
    this.selectTarget();
  }

  private createTracingSessionWrapper(
    target: RecordingTargetV2,
  ): TracingSessionWrapper {
    return new TracingSessionWrapper(target, this);
  }

  private clearRecordingState(): void {
    this.tracingSessionWrapper = undefined;
    this.setState(RecordingState.TARGET_INFO_DISPLAYED);
    this.recMgr.setRecordingStatus(undefined);
    // Redrawing because this method has changed the RecordingState, which will
    // affect the display of the record_page.
    this.app.raf.scheduleFullRedraw();
  }

  private setState(state: RecordingState) {
    this.state = state;
    this.stateGeneration += 1;
  }
}
