import {Disposable, Trash} from '../base/disposable';
import {assertExists} from '../base/logging';
import {uuidv4} from '../base/uuid';
import {BottomTabToSCSAdapter} from '../public';

import {globals} from './globals';
import {NotesEditorTab} from './notes_panel';

function getEngine() {
  const engineId = assertExists(globals.getCurrentEngine()).id;
  const engine = assertExists(globals.engines.get(engineId));
  return engine;
}

/**
 * Registers with the tab manager to show notes details panels when notes are
 * selected.
 *
 * Notes are core functionality thus don't really belong in a plugin.
 */
export class Notes implements Disposable {
  private trash = new Trash();

  constructor() {
    const unregister = globals.tabManager.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (selection) => {
          if (selection.kind === 'NOTE') {
            return new NotesEditorTab({
              config: {
                id: selection.id,
              },
              engine: getEngine().getProxy('Notes'),
              uuid: uuidv4(),
            });
          } else {
            return undefined;
          }
        },
      }));
    this.trash.add(unregister);
  }

  dispose(): void {
    this.trash.dispose();
  }
}
