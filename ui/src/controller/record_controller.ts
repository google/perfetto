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

import {Message, Method, rpc, RPCImplCallback} from 'protobufjs';
import {isString} from '../base/object_utils';
import {base64Encode} from '../base/string_utils';
import {Actions} from '../common/actions';
import {TRACE_SUFFIX} from '../common/constants';
import {genTraceConfig} from '../common/recordingV2/recording_config_utils';
import {TargetInfo} from '../common/recordingV2/recording_interfaces_v2';
import {
  AdbRecordingTarget,
  isAdbTarget,
  isChromeTarget,
  isWindowsTarget,
  RecordingTarget,
} from '../common/state';
import {globals} from '../frontend/globals';
import {publishBufferUsage, publishTrackData} from '../frontend/publish';
import {ConsumerPort, TraceConfig} from '../protos';
import {AdbOverWebUsb} from './adb';
import {AdbConsumerPort} from './adb_shell_controller';
import {AdbSocketConsumerPort} from './adb_socket_controller';
import {ChromeExtensionConsumerPort} from './chrome_proxy_record_controller';
import {
  ConsumerPortResponse,
  GetTraceStatsResponse,
  isDisableTracingResponse,
  isEnableTracingResponse,
  isFreeBuffersResponse,
  isGetTraceStatsResponse,
  isReadBuffersResponse,
} from './consumer_port_types';
import {Controller} from './controller';
import {RecordConfig} from './record_config_types';
import {Consumer, RpcConsumerPort} from './record_controller_interfaces';

type RPCImplMethod = Method | rpc.ServiceMethod<Message<{}>, Message<{}>>;

export function genConfigProto(
  uiCfg: RecordConfig,
  target: RecordingTarget,
): Uint8Array {
  return TraceConfig.encode(convertToRecordingV2Input(uiCfg, target)).finish();
}

// This method converts the 'RecordingTarget' to the 'TargetInfo' used by V2 of
// the recording code. It is used so the logic is not duplicated and does not
// diverge.
// TODO(octaviant) delete this once we switch to RecordingV2.
function convertToRecordingV2Input(
  uiCfg: RecordConfig,
  target: RecordingTarget,
): TraceConfig {
  let targetType: 'ANDROID' | 'CHROME' | 'CHROME_OS' | 'LINUX' | 'WINDOWS';
  let androidApiLevel!: number;
  switch (target.os) {
    case 'L':
      targetType = 'LINUX';
      break;
    case 'C':
      targetType = 'CHROME';
      break;
    case 'CrOS':
      targetType = 'CHROME_OS';
      break;
    case 'Win':
      targetType = 'WINDOWS';
      break;
    case 'S':
      androidApiLevel = 31;
      targetType = 'ANDROID';
      break;
    case 'R':
      androidApiLevel = 30;
      targetType = 'ANDROID';
      break;
    case 'Q':
      androidApiLevel = 29;
      targetType = 'ANDROID';
      break;
    case 'P':
      androidApiLevel = 28;
      targetType = 'ANDROID';
      break;
    default:
      androidApiLevel = 26;
      targetType = 'ANDROID';
  }

  let targetInfo: TargetInfo;
  if (targetType === 'ANDROID') {
    targetInfo = {
      targetType,
      androidApiLevel,
      dataSources: [],
      name: '',
    };
  } else {
    targetInfo = {
      targetType,
      dataSources: [],
      name: '',
    };
  }

  return genTraceConfig(uiCfg, targetInfo);
}

export function toPbtxt(configBuffer: Uint8Array): string {
  const msg = TraceConfig.decode(configBuffer);
  const json = msg.toJSON();
  function snakeCase(s: string): string {
    return s.replace(/[A-Z]/g, (c) => '_' + c.toLowerCase());
  }
  // With the ahead of time compiled protos we can't seem to tell which
  // fields are enums.
  function isEnum(value: string): boolean {
    return (
      value.startsWith('MEMINFO_') ||
      value.startsWith('VMSTAT_') ||
      value.startsWith('STAT_') ||
      value.startsWith('LID_') ||
      value.startsWith('BATTERY_COUNTER_') ||
      value === 'DISCARD' ||
      value === 'RING_BUFFER' ||
      value === 'BACKGROUND' ||
      value === 'USER_INITIATED' ||
      value.startsWith('PERF_CLOCK_')
    );
  }
  // Since javascript doesn't have 64 bit numbers when converting protos to
  // json the proto library encodes them as strings. This is lossy since
  // we can't tell which strings that look like numbers are actually strings
  // and which are actually numbers. Ideally we would reflect on the proto
  // definition somehow but for now we just hard code keys which have this
  // problem in the config.
  function is64BitNumber(key: string): boolean {
    return [
      'maxFileSizeBytes',
      'pid',
      'samplingIntervalBytes',
      'shmemSizeBytes',
      'timestampUnitMultiplier',
      'frequency',
    ].includes(key);
  }
  function* message(msg: {}, indent: number): IterableIterator<string> {
    for (const [key, value] of Object.entries(msg)) {
      const isRepeated = Array.isArray(value);
      const isNested = typeof value === 'object' && !isRepeated;
      for (const entry of isRepeated ? (value as Array<{}>) : [value]) {
        yield ' '.repeat(indent) + `${snakeCase(key)}${isNested ? '' : ':'} `;
        if (isString(entry)) {
          if (isEnum(entry) || is64BitNumber(key)) {
            yield entry;
          } else {
            yield `"${entry.replace(new RegExp('"', 'g'), '\\"')}"`;
          }
        } else if (typeof entry === 'number') {
          yield entry.toString();
        } else if (typeof entry === 'boolean') {
          yield entry.toString();
        } else if (typeof entry === 'object' && entry !== null) {
          yield '{\n';
          yield* message(entry, indent + 4);
          yield ' '.repeat(indent) + '}';
        } else {
          throw new Error(
            `Record proto entry "${entry}" with unexpected type ${typeof entry}`,
          );
        }
        yield '\n';
      }
    }
  }
  return [...message(json, 0)].join('');
}

export class RecordController extends Controller<'main'> implements Consumer {
  private config: RecordConfig | null = null;
  private readonly extensionPort: MessagePort;
  private recordingInProgress = false;
  private consumerPort: ConsumerPort;
  private traceBuffer: Uint8Array[] = [];
  private bufferUpdateInterval: ReturnType<typeof setTimeout> | undefined;
  private adb = new AdbOverWebUsb();
  private recordedTraceSuffix = TRACE_SUFFIX;
  private fetchedCategories = false;

  // We have a different controller for each targetOS. The correct one will be
  // created when needed, and stored here. When the key is a string, it is the
  // serial of the target (used for android devices). When the key is a single
  // char, it is the 'targetOS'
  private controllerPromises = new Map<string, Promise<RpcConsumerPort>>();

  constructor(args: {extensionPort: MessagePort}) {
    super('main');
    this.consumerPort = ConsumerPort.create(this.rpcImpl.bind(this));
    this.extensionPort = args.extensionPort;
  }

  run() {
    // TODO(eseckler): Use ConsumerPort's QueryServiceState instead
    // of posting a custom extension message to retrieve the category list.
    if (globals.state.fetchChromeCategories && !this.fetchedCategories) {
      this.fetchedCategories = true;
      if (globals.state.extensionInstalled) {
        this.extensionPort.postMessage({method: 'GetCategories'});
      }
      globals.dispatch(Actions.setFetchChromeCategories({fetch: false}));
    }
    if (
      globals.state.recordConfig === this.config &&
      globals.state.recordingInProgress === this.recordingInProgress
    ) {
      return;
    }
    this.config = globals.state.recordConfig;

    const configProto = genConfigProto(
      this.config,
      globals.state.recordingTarget,
    );
    const configProtoText = toPbtxt(configProto);
    const configProtoBase64 = base64Encode(configProto);
    const commandline = `
      echo '${configProtoBase64}' |
      base64 --decode |
      adb shell "perfetto -c - -o /data/misc/perfetto-traces/trace" &&
      adb pull /data/misc/perfetto-traces/trace /tmp/trace
    `;
    const traceConfig = convertToRecordingV2Input(
      this.config,
      globals.state.recordingTarget,
    );
    // TODO(hjd): This should not be TrackData after we unify the stores.
    publishTrackData({
      id: 'config',
      data: {
        commandline,
        pbBase64: configProtoBase64,
        pbtxt: configProtoText,
        traceConfig,
      },
    });

    // If the recordingInProgress boolean state is different, it means that we
    // have to start or stop recording a trace.
    if (globals.state.recordingInProgress === this.recordingInProgress) return;
    this.recordingInProgress = globals.state.recordingInProgress;

    if (this.recordingInProgress) {
      this.startRecordTrace(traceConfig);
    } else {
      this.stopRecordTrace();
    }
  }

  startRecordTrace(traceConfig: TraceConfig) {
    this.scheduleBufferUpdateRequests();
    this.traceBuffer = [];
    this.consumerPort.enableTracing({traceConfig});
  }

  stopRecordTrace() {
    if (this.bufferUpdateInterval) clearInterval(this.bufferUpdateInterval);
    this.consumerPort.disableTracing({});
  }

  scheduleBufferUpdateRequests() {
    if (this.bufferUpdateInterval) clearInterval(this.bufferUpdateInterval);
    this.bufferUpdateInterval = setInterval(() => {
      this.consumerPort.getTraceStats({});
    }, 200);
  }

  readBuffers() {
    this.consumerPort.readBuffers({});
  }

  onConsumerPortResponse(data: ConsumerPortResponse) {
    if (data === undefined) return;
    if (isReadBuffersResponse(data)) {
      if (!data.slices || data.slices.length === 0) return;
      // TODO(nicomazz): handle this as intended by consumer_port.proto.
      console.assert(data.slices.length === 1);
      if (data.slices[0].data) this.traceBuffer.push(data.slices[0].data);
      // The line underneath is 'misusing' the format ReadBuffersResponse.
      // The boolean field 'lastSliceForPacket' is used as 'lastPacketInTrace'.
      // See http://shortn/_53WB8A1aIr.
      if (data.slices[0].lastSliceForPacket) this.onTraceComplete();
    } else if (isEnableTracingResponse(data)) {
      this.readBuffers();
    } else if (isGetTraceStatsResponse(data)) {
      const percentage = this.getBufferUsagePercentage(data);
      if (percentage) {
        publishBufferUsage({percentage});
      }
    } else if (isFreeBuffersResponse(data)) {
      // No action required.
    } else if (isDisableTracingResponse(data)) {
      // No action required.
    } else {
      console.error('Unrecognized consumer port response:', data);
    }
  }

  onTraceComplete() {
    this.consumerPort.freeBuffers({});
    globals.dispatch(Actions.setRecordingStatus({status: undefined}));
    if (globals.state.recordingCancelled) {
      globals.dispatch(
        Actions.setLastRecordingError({error: 'Recording cancelled.'}),
      );
      this.traceBuffer = [];
      return;
    }
    const trace = this.generateTrace();
    globals.dispatch(
      Actions.openTraceFromBuffer({
        title: 'Recorded trace',
        buffer: trace.buffer,
        fileName: `recorded_trace${this.recordedTraceSuffix}`,
      }),
    );
    this.traceBuffer = [];
  }

  // TODO(nicomazz): stream each chunk into the trace processor, instead of
  // creating a big long trace.
  generateTrace() {
    let traceLen = 0;
    for (const chunk of this.traceBuffer) traceLen += chunk.length;
    const completeTrace = new Uint8Array(traceLen);
    let written = 0;
    for (const chunk of this.traceBuffer) {
      completeTrace.set(chunk, written);
      written += chunk.length;
    }
    return completeTrace;
  }

  getBufferUsagePercentage(data: GetTraceStatsResponse): number {
    if (!data.traceStats || !data.traceStats.bufferStats) return 0.0;
    let maximumUsage = 0;
    for (const buffer of data.traceStats.bufferStats) {
      const used = buffer.bytesWritten as number;
      const total = buffer.bufferSize as number;
      maximumUsage = Math.max(maximumUsage, used / total);
    }
    return maximumUsage;
  }

  onError(message: string) {
    // TODO(octaviant): b/204998302
    console.error('Error in record controller: ', message);
    globals.dispatch(
      Actions.setLastRecordingError({error: message.substr(0, 150)}),
    );
    globals.dispatch(Actions.stopRecording({}));
  }

  onStatus(message: string) {
    globals.dispatch(Actions.setRecordingStatus({status: message}));
  }

  // Depending on the recording target, different implementation of the
  // consumer_port will be used.
  // - Chrome target: This forwards the messages that have to be sent
  // to the extension to the frontend. This is necessary because this
  // controller is running in a separate worker, that can't directly send
  // messages to the extension.
  // - Android device target: WebUSB is used to communicate using the adb
  // protocol. Actually, there is no full consumer_port implementation, but
  // only the support to start tracing and fetch the file.
  async getTargetController(target: RecordingTarget): Promise<RpcConsumerPort> {
    const identifier = RecordController.getTargetIdentifier(target);

    // The reason why caching the target 'record controller' Promise is that
    // multiple rcp calls can happen while we are trying to understand if an
    // android device has a socket connection available or not.
    const precedentPromise = this.controllerPromises.get(identifier);
    if (precedentPromise) return precedentPromise;

    const controllerPromise = new Promise<RpcConsumerPort>(
      async (resolve, _) => {
        let controller: RpcConsumerPort | undefined = undefined;
        if (isChromeTarget(target) || isWindowsTarget(target)) {
          controller = new ChromeExtensionConsumerPort(
            this.extensionPort,
            this,
          );
        } else if (isAdbTarget(target)) {
          this.onStatus(`Please allow USB debugging on device.
                 If you press cancel, reload the page.`);
          const socketAccess = await this.hasSocketAccess(target);

          controller = socketAccess
            ? new AdbSocketConsumerPort(this.adb, this)
            : new AdbConsumerPort(this.adb, this);
        } else {
          throw Error(`No device connected`);
        }

        /* eslint-disable @typescript-eslint/strict-boolean-expressions */
        if (!controller) throw Error(`Unknown target: ${target}`);
        /* eslint-enable */
        resolve(controller);
      },
    );

    this.controllerPromises.set(identifier, controllerPromise);
    return controllerPromise;
  }

  private static getTargetIdentifier(target: RecordingTarget): string {
    return isAdbTarget(target) ? target.serial : target.os;
  }

  private async hasSocketAccess(target: AdbRecordingTarget) {
    const devices = await navigator.usb.getDevices();
    const device = devices.find((d) => d.serialNumber === target.serial);
    console.assert(device);
    if (!device) return Promise.resolve(false);
    return AdbSocketConsumerPort.hasSocketAccess(device, this.adb);
  }

  private async rpcImpl(
    method: RPCImplMethod,
    requestData: Uint8Array,
    _callback: RPCImplCallback,
  ) {
    try {
      const state = globals.state;
      // TODO(hjd): This is a bit weird. We implicitly send each RPC message to
      // whichever target is currently selected (creating that target if needed)
      // it would be nicer if the setup/teardown was more explicit.
      const target = await this.getTargetController(state.recordingTarget);
      this.recordedTraceSuffix = target.getRecordedTraceSuffix();
      target.handleCommand(method.name, requestData);
    } catch (e) {
      console.error(`error invoking ${method}: ${e.message}`);
    }
  }
}
