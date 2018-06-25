export = InitWasm;

declare function InitWasm(_: InitWasm.Module): void;

// See https://kripken.github.io/emscripten-site/docs/api_reference/module.html
declare namespace InitWasm {
  export interface Module {
    locateFile(s: string): string;
    print(s: string): void;
    printErr(s: string): void;
  }
}
