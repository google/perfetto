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

import {TraceConfig} from '../../protos';

// TargetFactory connects, disconnects and keeps track of targets.
// There is one factory for AndroidWebusb, AndroidWebsocket, Chrome etc.
// For instance, the AndroidWebusb factory returns a RecordingTargetV2 for each
// device.
export interface TargetFactory {
  // Store the kind explicitly as a string as opposed to using class.kind in
  // case we ever minify our code.
  readonly kind: string;

  // Setter for OnTargetChange, which is executed when a target is
  // added/removed or when its information is updated.
  setOnTargetChange(onTargetChange: OnTargetChangeCallback): void;

  getName(): string;

  listTargets(): RecordingTargetV2[];
  // Returns recording problems that we encounter when not directly using the
  // target. For instance we connect webusb devices when Perfetto is loaded. If
  // there is an issue with connecting a webusb device, we do not want to crash
  // all of Perfetto, as the user may not want to use the recording
  // functionality at all.
  listRecordingProblems(): string[];

  connectNewTarget(): Promise<RecordingTargetV2>;
}

export interface DataSource {
  name: string;

  // Contains information that is opaque to the recording code. The caller can
  // use the DataSource name to type cast the DataSource descriptor.
  // For targets calling QueryServiceState, 'descriptor' will hold the
  // datasource descriptor:
  // https://source.corp.google.com/android/external/perfetto/protos/perfetto/
  // common/data_source_descriptor.proto;l=28-60
  // For Chrome, 'descriptor' will contain the answer received from
  // 'GetCategories':
  // https://source.corp.google.com/android/external/perfetto/ui/src/
  // chrome_extension/chrome_tracing_controller.ts;l=220
  descriptor: unknown;
}

// Common fields for all types of targetInfo: Chrome, Android, Linux etc.
interface TargetInfoBase {
  name: string;

  // The dataSources exposed by a target. They are fetched from the target
  // (ex: using QSS for Android or GetCategories for Chrome).
  dataSources: DataSource[];
}

export interface AndroidTargetInfo extends TargetInfoBase {
  targetType: 'ANDROID';

  // This is the Android API level. For instance, it can be 32, 31, 30 etc.
  // It is the "API level" column here:
  // https://source.android.com/setup/start/build-numbers
  androidApiLevel?: number;
}

export interface ChromeTargetInfo extends TargetInfoBase {
  targetType: 'CHROME' | 'CHROME_OS' | 'WINDOWS';
}

export interface HostOsTargetInfo extends TargetInfoBase {
  targetType: 'LINUX' | 'MACOS';
}

// Holds information about a target. It's used by the UI and the logic which
// generates a config.
export type TargetInfo =
  | AndroidTargetInfo
  | ChromeTargetInfo
  | HostOsTargetInfo;

// RecordingTargetV2 is subclassed by Android devices and the Chrome browser/OS.
// It creates tracing sessions which are used by the UI. For Android, it manages
// the connection with the device.
export interface RecordingTargetV2 {
  // Allows targets to surface target specific information such as
  // well known key/value pairs: OS, targetType('ANDROID', 'CHROME', etc.)
  getInfo(): TargetInfo;

  // Disconnects the target.
  disconnect(disconnectMessage?: string): Promise<void>;

  // Returns true if we are able to connect to the target without interfering
  // with other processes. For example, for adb devices connected over WebUSB,
  // this will be false when we can not claim the interface (Which most likely
  // means that 'adb server' is running locally.). After querrying this method,
  // the caller can decide if they want to connect to the target and as a side
  // effect take the connection away from other processes.
  canConnectWithoutContention(): Promise<boolean>;

  // Whether the recording target can be used in a tracing session. For example,
  // virtual targets do not support a tracing session.
  canCreateTracingSession(recordingMode?: string): boolean;

  // Some target information can only be obtained after connecting to the
  // target. This will establish a connection and retrieve data such as
  // dataSources and apiLevel for Android.
  fetchTargetInfo(
    tracingSessionListener: TracingSessionListener,
  ): Promise<void>;

  createTracingSession(
    tracingSessionListener: TracingSessionListener,
  ): Promise<TracingSession>;
}

// TracingSession is used by the UI to record a trace. Depending on user
// actions, the UI can start/stop/cancel a session. During the recording, it
// provides updates about buffer usage. It is subclassed by
// TracedTracingSession, which manages the communication with traced and has
// logic for encoding/decoding Perfetto client requests/replies.
export interface TracingSession {
  // Starts the tracing session.
  start(config: TraceConfig): void;

  // Will stop the tracing session and NOT return any trace.
  cancel(): void;

  // Will stop the tracing session. The implementing class may also return
  // the trace using a callback.
  stop(): void;

  // Returns the percentage of the trace buffer that is currently being
  // occupied.
  getTraceBufferUsage(): Promise<number>;
}

// Connection with an Adb device. Implementations will have logic specific to
// the connection protocol used(Ex: WebSocket, WebUsb).
export interface AdbConnection {
  // Will push a binary to a given path.
  push(binary: ArrayBuffer, path: string): Promise<void>;

  // Will issue a shell command to the device.
  shell(cmd: string): Promise<ByteStream>;

  // Will establish a connection(a ByteStream) with the device.
  connectSocket(path: string): Promise<ByteStream>;

  // Returns true if we are able to connect without interfering
  // with other processes. For example, for adb devices connected over WebUSB,
  // this will be false when we can not claim the interface (Which most likely
  // means that 'adb server' is running locally.).
  canConnectWithoutContention(): Promise<boolean>;

  // Ends the connection.
  disconnect(disconnectMessage?: string): Promise<void>;
}

// A stream for a connection between a target and a tracing session.
export interface ByteStream {
  // The caller can add callbacks, to be executed when the stream receives new
  // data or when it finished closing itself.
  addOnStreamDataCallback(onStreamData: OnStreamDataCallback): void;
  addOnStreamCloseCallback(onStreamClose: OnStreamCloseCallback): void;

  isConnected(): boolean;
  write(data: string | Uint8Array): void;

  close(): void;
  closeAndWaitForTeardown(): Promise<void>;
}

// Handles binary messages received over the ByteStream.
export interface OnStreamDataCallback {
  (data: Uint8Array): void;
}

// Called when the ByteStream is closed.
export interface OnStreamCloseCallback {
  (): void;
}

// OnTraceDataCallback will return the entire trace when it has been fully
// assembled. This will be changed in the following CL aosp/2057640.
export interface OnTraceDataCallback {
  (trace: Uint8Array): void;
}

// Handles messages that are useful in the UI and that occur at any layer of the
// recording (trace, connection). The messages includes both status messages and
// error messages.
export interface OnMessageCallback {
  (message: string): void;
}

// Handles the loss of the connection at the connection layer (used by the
// AdbConnection).
export interface OnDisconnectCallback {
  (errorMessage?: string): void;
}

// Called when there is a change of targets or within a target.
// For instance, it's used when an Adb device becomes connected/disconnected.
// It's also executed by a target when the information it stores gets updated.
export interface OnTargetChangeCallback {
  (): void;
}

// A collection of callbacks that is passed to RecordingTargetV2 and
// subsequently to TracingSession. The callbacks are decided by the UI, so the
// recording code is not coupled with the rendering logic.
export interface TracingSessionListener {
  onTraceData: OnTraceDataCallback;
  onStatus: OnMessageCallback;
  onDisconnect: OnDisconnectCallback;
  onError: OnMessageCallback;
}
