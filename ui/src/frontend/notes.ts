import {Disposable, Trash} from '../base/disposable';

import {globals} from './globals';
import {NotesEditorTab} from './notes_panel';

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
      new NotesEditorTab(),
    );
    this.trash.add(unregister);
  }

  dispose(): void {
    this.trash.dispose();
  }
}
