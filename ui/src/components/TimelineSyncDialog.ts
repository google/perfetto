import m from 'mithril';

interface TimelineSyncDialogAttrs {
  disableTimelineSync: () => void;
}

export class TimelineSyncDialog implements m.ClassComponent<TimelineSyncDialogAttrs> {
  view({attrs}: m.CVnode<TimelineSyncDialogAttrs>) {
    return m(
      'div.timeline-sync-dialog',
      {
        style: {
          position: 'absolute',
          bottom: '20px',
          right: '20px',
          backgroundColor: 'white',
          border: '1px solid gray',
          padding: '10px',
          zIndex: 1000, // Ensure it's on top of other elements
        },
      },
      [
        m('span', 'Timeline sync is enabled.'),
        m(
          'button',
          {
            style: {marginLeft: '10px'},
            onclick: attrs.disableTimelineSync,
          },
          'Disable',
        ),
      ],
    );
  }
}
