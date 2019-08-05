import Protocol from 'devtools-protocol';
// TODO(nicomazz): Use noice-json-rpc.
import {TraceConfig} from '../common/protos';

const CHUNK_SIZE: number = 1024 * 1024 * 64;

export class ChromeTraceController {
  recordingTarget: chrome.debugger.Debuggee|null = null;
  streamHandle: string|undefined = undefined;
  startTracingSendResponse: Function|null = null;
  dataBuffer = '';

  constructor() {
    chrome.debugger.onEvent.addListener(this.onEvent.bind(this));
    chrome.debugger.onDetach.addListener(this.onDetach.bind(this));
  }

  onMessage(
      request: {method: string, traceConfig: Uint8Array},
      _sender: chrome.runtime.MessageSender, sendResponse: Function) {
    console.log('OnMessage: ', request);
    switch (request.method) {
      case 'EnableTracing':
        this.cleanState();
        this.startTracingSendResponse = sendResponse;
        const traceConfig =
            TraceConfig.decode(new Uint8Array(request.traceConfig));
        this.handleStartTracing(traceConfig);
        break;
      case 'StartTracing':
        sendResponse({
          answer: 'no-op, start tracing happens when enable tracing is called.'
        });
        break;
      case 'FreeBuffers':
        sendResponse({answer: 'Freeing buffers'});
        this.handleFreeBuffers();
        break;
      case 'ReadBuffers':
        this.handleReadBuffers(sendResponse);
        break;
      case 'StopTracing':
        this.startTracingSendResponse = sendResponse;
        this.handleStopTracing();
        break;
      default:
        sendResponse({error: 'Action not recognised'});
        console.log('Received not recognized message');
        break;
    }
  }

  cleanState() {
    this.recordingTarget = null;
    this.streamHandle = undefined;
    this.startTracingSendResponse = null;
    this.dataBuffer = '';
  }

  onEvent(_source: chrome.debugger.Debuggee, method: string, params?: object) {
    if (method === 'Tracing.tracingComplete' && params) {
      this.streamHandle =
          (params as Protocol.Tracing.TracingCompleteEvent).stream;
      this.notifyFrontendStreamReady();
    }
  }

  notifyFrontendStreamReady() {
    // TODO(nicomazz): Send the response in a separate way.
    if (this.startTracingSendResponse) {
      this.startTracingSendResponse({recordingReady: 1});
    }
  }

  handleStartTracing(traceConfig: TraceConfig) {
    const args: Protocol.Tracing.StartRequest = {
      traceConfig: {includedCategories: []},
      streamFormat: 'proto',
      transferMode: 'ReturnAsStream',
      streamCompression: 'gzip'
    };
    this.findAndAttachTarget(t => {
      chrome.debugger.sendCommand(t, 'Tracing.start', args, results => {
        console.log('tracing started with config:', traceConfig, results);
        // For initial testing, the recording is stopped after 3 seconds.
        setTimeout(() => {
          this.handleStopTracing();
        }, 3000);
      });
    });
  }

  findTarget(then: (target: chrome.debugger.Debuggee) => void) {
    chrome.debugger.getTargets(targets => {
      const perfettoTab =
          targets.find((target) => target.title.includes('Perfetto'));
      if (perfettoTab === undefined) {
        console.log('No perfetto tab found');
        return;
      }
      const t: chrome.debugger.Debuggee = {targetId: perfettoTab.id};
      this.recordingTarget = t;
      then(t);
    });
  }

  findAndAttachTarget(then: (target: chrome.debugger.Debuggee) => void) {
    this.findTarget(t => {
      chrome.debugger.attach(t, /*requiredVersion=*/ '1.3', () => {
        then(t);
      });
    });
  }

  handleStopTracing() {
    if (this.recordingTarget === null) {
      console.log('No recordings in progress');
      return;
    }
    chrome.debugger.sendCommand(
        this.recordingTarget, 'Tracing.end', undefined, _ => {});
  }

  handleFreeBuffers() {
    if (this.recordingTarget !== null) {
      chrome.debugger.detach(this.recordingTarget, () => {
        this.recordingTarget = null;
      });
    }
  }

  handleReadBuffers(sendResponse: Function, offset = 0) {
    // TODO(nicomazz): Send back the response each time a chunk is readed.
    if (this.recordingTarget === null || this.streamHandle === undefined) {
      return;
    }
    this.readBuffer(this.streamHandle, offset, res => {
      if (res === undefined) return;
      const chunk = res.base64Encoded ? atob(res.data) : res.data;
      this.dataBuffer += chunk;
      if (res.eof) {
        sendResponse({traceData: this.dataBuffer});
        return;
      }
      this.handleReadBuffers(sendResponse, offset + res.data.length);
    });
  }

  readBuffer(
      handle: string, offset: number,
      then: (res: Protocol.IO.ReadResponse) => void) {
    if (this.recordingTarget === null) return;
    const readRequest:
        Protocol.IO.ReadRequest = {handle, offset, size: CHUNK_SIZE};
    chrome.debugger.sendCommand(
        this.recordingTarget,
        'IO.read',
        readRequest,
        res => then(res as Protocol.IO.ReadResponse));
  }

  onDetach(source: chrome.debugger.Debuggee, reason: string) {
    console.log('source detached: ', source, 'reason: ', reason);
    this.recordingTarget = null;
    this.streamHandle = undefined;
  }
}