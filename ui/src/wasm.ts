import * as init_trace_processor from './gen/trace_processor';

function writeToUIConsole(line:string) {
  const lineElement = document.createElement('div');
  lineElement.innerText = line;
  const container = document.getElementById('console');
  if (!container)
    throw new Error('OMG');
  container.appendChild(lineElement);
}

init_trace_processor({
  locateFile: (s: string) => s,
  print: writeToUIConsole,
  printErr: writeToUIConsole,
});

