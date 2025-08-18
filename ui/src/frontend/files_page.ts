import m from 'mithril';
import {createPage} from './pages';

interface FilesPageState {
  files: string[];
  loading: boolean;
  error: string | null;
}

const state: FilesPageState = {
  files: [],
  loading: false,
  error: null,
};

async function fetchFileList() {
  state.loading = true;
  state.error = null;
  
  const URL = `http://${window.location.hostname}:9001/getFileList`;
  try {
    const response = await fetch(URL, {method: 'GET'});
    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }
    const text = await response.text();
    state.files = text.split('\n').filter(f => f.trim() !== '');
  } catch (error) {
    state.error = error instanceof Error ? error.message : 'Unknown error';
    state.files = [];
  } finally {
    state.loading = false;
  }
}

class FilesPageContents implements m.ClassComponent {
  oncreate() {
    fetchFileList();
  }

  view() {
    return m('.files-page',
      m('.header-section',
        m('h1.page-title', 
          m('span.icon.material-icons', 'folder'),
          'Trace Files'
        ),
        m('p.page-subtitle', 'Browse and open available trace files'),
      ),
      
      state.loading && 
        m('.loading-container',
          m('.spinner'),
          m('p', 'Loading files...')
        ),
      
      state.error && 
        m('.error-container',
          m('.error-icon.material-icons', 'error_outline'),
          m('h3', 'Failed to load files'),
          m('p', state.error),
          m('button.retry-btn', {onclick: fetchFileList}, 
            m('span.icon.material-icons', 'refresh'),
            'Try Again'
          )
        ),
      
      !state.loading && !state.error && state.files.length === 0 &&
        m('.empty-container',
          m('.empty-icon.material-icons', 'folder_open'),
          m('h3', 'No files found'),
          m('p', 'There are no trace files available in the system. Try refreshing the page.')
        ),
      
      !state.loading && !state.error && state.files.length > 0 &&
        m('.files-grid',
          m('.files-header',
            m('.header-item', 'File Name'),
            m('.header-item', 'Size'),
            m('.header-item', 'Actions')
          ),
          m('.files-list',
            state.files.map((filename, index) => 
              m('.file-item', {
                key: filename,
                class: index % 2 === 0 ? 'even' : 'odd'
              },
                m('.file-info',
                  m('.file-icon.material-icons', 'description'),
                  m('.file-name', filename)
                ),
                m('.file-size', '--'),
                m('.file-actions',
                  m('button.open-btn', {
                    onclick: () => {
                      window.location.href = `#!/viewer?storage=${encodeURIComponent(filename)}`;
                    }
                  },
                    m('span.icon.material-icons', 'open_in_new'),
                    'Open'
                  )
                )
              )
            )
          )
        )
    );
  }
}

export const FilesPage = createPage({
  view() {
    return m('.files-page-container', 
      m('style', `
        .files-page-container {
          padding: 24px;
          max-width: 1200px;
          margin: 0 auto;
          font-family: 'Roboto', 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
        }

        .header-section {
          margin-bottom: 32px;
        }

        .page-title {
          font-size: 28px;
          font-weight: 300;
          color: #202124;
          margin: 0 0 8px 0;
          display: flex;
          align-items: center;
          gap: 12px;
        }

        .page-title .icon {
          font-size: 32px;
          color: #1a73e8;
        }

        .page-subtitle {
          font-size: 16px;
          color: #5f6368;
          margin: 0 0 16px 0;
        }

        .refresh-btn, .retry-btn, .open-btn {
          background: #1a73e8;
          color: white;
          border: none;
          padding: 8px 16px;
          border-radius: 4px;
          cursor: pointer;
          font-size: 14px;
          display: inline-flex;
          align-items: center;
          gap: 8px;
          transition: background-color 0.2s;
        }

        .refresh-btn:hover, .retry-btn:hover, .open-btn:hover {
          background: #1557b0;
        }

        .refresh-btn:disabled {
          background: #bdc1c6;
          cursor: not-allowed;
        }

        .loading-container, .error-container, .empty-container {
          text-align: center;
          padding: 48px 24px;
          background: white;
          border-radius: 8px;
          box-shadow: 0 1px 3px rgba(0,0,0,0.12);
        }

        .spinner {
          border: 3px solid #f3f3f3;
          border-top: 3px solid #1a73e8;
          border-radius: 50%;
          width: 40px;
          height: 40px;
          animation: spin 1s linear infinite;
          margin: 0 auto 16px;
        }

        @keyframes spin {
          0% { transform: rotate(0deg); }
          100% { transform: rotate(360deg); }
        }

        .error-icon, .empty-icon {
          font-size: 48px;
          color: #dadce0;
          margin-bottom: 16px;
        }

        .error-icon {
          color: #d93025;
        }

        .files-grid {
          background: white;
          border-radius: 8px;
          box-shadow: 0 1px 3px rgba(0,0,0,0.12);
          overflow: hidden;
        }

        .files-header {
          display: grid;
          grid-template-columns: 1fr 100px 120px;
          background: #f8f9fa;
          padding: 12px 16px;
          font-weight: 500;
          color: #5f6368;
          font-size: 14px;
        }

        .files-list {
          max-height: 600px;
          overflow-y: auto;
        }

        .file-item {
          display: grid;
          grid-template-columns: 1fr 100px 120px;
          align-items: center;
          padding: 12px 16px;
          border-bottom: 1px solid #f0f0f0;
          transition: background-color 0.2s;
        }

        .file-item:hover {
          background-color: #f8f9fa;
        }

        .file-item.even {
          background-color: #fafafa;
        }

        .file-info {
          display: flex;
          align-items: center;
          gap: 12px;
        }

        .file-icon {
          color: #1a73e8;
          font-size: 20px;
        }

        .file-name {
          font-size: 14px;
          color: #202124;
          font-weight: 500;
        }

        .file-size {
          font-size: 14px;
          color: #5f6368;
        }

        .file-actions {
          display: flex;
          justify-content: flex-end;
        }

        .open-btn {
          background: transparent;
          color: #1a73e8;
          border: 1px solid #dadce0;
          padding: 6px 12px;
          font-size: 13px;
        }

        .open-btn:hover {
          background: #f8f9fa;
          border-color: #1a73e8;
        }

        .open-btn .icon {
          font-size: 16px;
        }
      `),
      m(FilesPageContents)
    );
  },
});