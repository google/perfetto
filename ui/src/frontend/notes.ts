import {Disposable, Trash} from '../base/disposable';

import {globals} from './globals';
import {NotesManager} from './notes_manager';
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
    this.trash.add(
      globals.tabManager.registerDetailsPanel(new NotesEditorTab()),
    );

    this.trash.add(
      globals.tabManager.registerTab({
        uri: 'notes.manager',
        isEphemeral: false,
        content: {
          getTitle: () => 'Notes & markers',
          render: () => m(NotesManager),
        },
      }),
    );
  }

  dispose(): void {
    this.trash.dispose();
  }
}
