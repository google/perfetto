const engineReadyObservers: (() => void)[] = [];

export function onEngineReady() {
  for (const observer of engineReadyObservers) {
    observer();
  }
}

export function addEngineReadyObserver(observer: ()=>void):
    void {
        engineReadyObservers.push(observer);
}