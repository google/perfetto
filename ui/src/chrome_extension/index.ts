import {TraceConfig} from '../common/protos';
console.log(TraceConfig);

chrome.runtime.onInstalled.addListener(() => {
  enableOnlyOnPerfettoHost();
  chrome.runtime.onMessageExternal.addListener(onMessage);
  chrome.debugger.onEvent.addListener(onEvent);
});

function onMessage(
    request: {method: string, traceConfig: TraceConfig},
    _sender: chrome.runtime.MessageSender,
    sendResponse: Function) {
  console.log('OnMessage: ');
  console.log(request);
  switch (request.method) {
    case 'ExtensionVersion':
      sendResponse({version: chrome.runtime.getManifest().version});
      break;
    case 'EnableTracing':
      // TODO(nicomazz): Send proper responses for Enable/Start/StopTracing.
      sendResponse({answer: 'starting tracing!'});
      handleStartTracing(request.traceConfig);
      break;
    case 'StartTracing':
      sendResponse({
        answer: 'no-op, start tracing happens when enable tracing is called'
      });
      break;
    case 'StopTracing':
      sendResponse({answer: 'stopping tracing!'});
      handleStopTracing();
      break;
    default:
      sendResponse({error: 'Action not recognised'});
      console.log('Received not recognized message');
      break;
  }
}

function onEvent(
    _source: chrome.debugger.Debuggee,
    method: string,
    params: object|undefined) {
  // TODO(nicomazz): Handle Tracing domain events.
  console.log('event received: ' + method);
  console.log('params: ', params);
}


let recordingTarget: chrome.debugger.Debuggee|null = null;

function handleStartTracing(traceConfig: TraceConfig) {
  findAndAttachTarget(t => {
    chrome.debugger.sendCommand(
        t,
        'Tracing.start',
        {traceConfig, streamFormat: 'proto', transferMode: 'ReturnAsStream'},
        results => {
          console.log('tracing started with config:');
          console.log(traceConfig);
          console.log(results);

          // only for initial testing
          setTimeout(() => {
            handleStopTracing();
          }, 3000);
        });
  });
}

function findTarget(then: (target: chrome.debugger.Debuggee) => void) {
  chrome.debugger.getTargets((targets) => {
    const perfettoTab =
        targets.find((target) => target.title.indexOf('Perfetto') !== -1);
    if (perfettoTab === undefined) {
      console.log('No perfetto tab found');
      return;
    }
    const t: chrome.debugger.Debuggee = {targetId: perfettoTab.id};
    recordingTarget = t;
    then(t);
  });
}

// TODO(nicomazz): Handle chrome.debugger.onDetach events.
function findAndAttachTarget(then: (target: chrome.debugger.Debuggee) => void) {
  findTarget((t) => {
    chrome.debugger.attach(t, /*requiredVersion=*/ '1.3', () => {
      then(t);
    });
  });
}

function handleStopTracing() {
  if (recordingTarget === null || recordingTarget === undefined) {
    console.log('No recordings in progress');
    return;
  }
  chrome.debugger.sendCommand(
      recordingTarget, 'Tracing.end', undefined, results => {
        console.log('tracing stopped:');
        console.log(results!.toString());
        recordingTarget = null;
      });
}


function enableOnlyOnPerfettoHost() {
  chrome.declarativeContent.onPageChanged.removeRules(undefined, () => {
    chrome.declarativeContent.onPageChanged.addRules([{
      conditions: [new chrome.declarativeContent.PageStateMatcher({
        // TODO(nicomazz): Also enable on ui.perfetto.dev once we're ready.
        pageUrl: {hostContains: 'perfetto.local'},
      })],
      actions: [new chrome.declarativeContent.ShowPageAction()]
    }]);
  });
}
