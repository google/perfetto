// import {TraceConfig} from '../common/protos';
import {ChromeTraceController} from './chrome_tracing_controller';

const chromeTraceController = new ChromeTraceController();

chrome.runtime.onInstalled.addListener(() => {
  enableOnlyOnPerfettoHost();
  // Listen for messages from the perfetto ui.
  chrome.runtime.onMessageExternal.addListener(onMessage);
});

function onMessage(
    request: {method: string, traceConfig: Uint8Array},
    _sender: chrome.runtime.MessageSender,
    sendResponse: Function) {
  if (request.method === 'ExtensionVersion') {
    sendResponse({version: chrome.runtime.getManifest().version});
    return;
  }
  // In the future more targets will be supported.
  chromeTraceController.onMessage(request, _sender, sendResponse);
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
