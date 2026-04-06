import React, { useState, useEffect, useCallback } from 'react';
import './App.css';

const API = process.env.REACT_APP_BACKEND_URL;

function FileTree({ files, selectedPath, onSelect }) {
  const grouped = {};
  files.forEach(f => {
    const parts = f.path.split('/');
    const dir = parts.length > 1 ? parts.slice(0, -1).join('/') : 'root';
    if (!grouped[dir]) grouped[dir] = [];
    grouped[dir].push(f);
  });

  const dirOrder = ['root', 'docs', 'src', 'src/usb', 'src/protocol', 'src/video', 'src/display', 'src/input', 'src/tls', 'src/util', 'companion-app', 'tools'];

  const sortedDirs = Object.keys(grouped).sort((a, b) => {
    const ai = dirOrder.indexOf(a);
    const bi = dirOrder.indexOf(b);
    if (ai >= 0 && bi >= 0) return ai - bi;
    if (ai >= 0) return -1;
    if (bi >= 0) return 1;
    return a.localeCompare(b);
  });

  const dirLabels = {
    'root': 'Project Root',
    'docs': 'Documentation',
    'src': 'Source (Core)',
    'src/usb': 'USB / AOA Layer',
    'src/protocol': 'Protocol Layer',
    'src/video': 'Video Decode (MJPEG)',
    'src/display': 'Display Output',
    'src/input': 'Touch Input',
    'src/tls': 'TLS (Stub)',
    'src/util': 'Utilities',
    'companion-app': 'Android Companion',
    'tools': 'Dev Tools',
  };

  const getIcon = (f) => {
    if (f.is_doc) return '\u{1F4D6}';
    if (f.name === 'Makefile') return '\u{2699}';
    if (f.name.endsWith('.h')) return '\u{1F4CB}';
    if (f.name.endsWith('.c')) return '\u{1F4BE}';
    return '\u{1F4C4}';
  };

  return (
    <div className="file-tree" data-testid="file-tree">
      {sortedDirs.map(dir => (
        <div key={dir} className="tree-section">
          <div className="tree-dir" data-testid={`dir-${dir.replace(/\//g, '-')}`}>
            {dirLabels[dir] || dir}
          </div>
          {grouped[dir].map(f => (
            <button
              key={f.path}
              className={`tree-file ${selectedPath === f.path ? 'selected' : ''} ${f.is_doc ? 'is-doc' : ''} ${f.is_source ? 'is-source' : ''}`}
              onClick={() => onSelect(f.path)}
              data-testid={`file-${f.path.replace(/[\/.]/g, '-')}`}
            >
              <span className="file-icon">{getIcon(f)}</span>
              <span className="file-name">{f.name}</span>
              <span className="file-size">{f.size > 1024 ? `${(f.size/1024).toFixed(1)}KB` : `${f.size}B`}</span>
            </button>
          ))}
        </div>
      ))}
    </div>
  );
}

function CodeViewer({ content, lang, path }) {
  if (!content) {
    return (
      <div className="code-viewer empty" data-testid="code-viewer-empty">
        <div className="empty-state">
          <div className="empty-icon">{'<'}/{'>'}</div>
          <h2>MN1 Android Auto Client</h2>
          <p>Select a file from the tree to view its contents.</p>
          <p className="hint">Start with <strong>README.md</strong> for the project overview.</p>
        </div>
      </div>
    );
  }

  if (lang === 'markdown') {
    return (
      <div className="code-viewer markdown" data-testid="code-viewer-md">
        <div className="viewer-header">
          <span className="viewer-path" data-testid="viewer-path">{path}</span>
          <span className="viewer-lang">MARKDOWN</span>
        </div>
        <div className="md-content">
          <MarkdownRenderer content={content} />
        </div>
      </div>
    );
  }

  const lines = content.split('\n');
  return (
    <div className="code-viewer source" data-testid="code-viewer-source">
      <div className="viewer-header">
        <span className="viewer-path" data-testid="viewer-path">{path}</span>
        <span className="viewer-lang">{lang.toUpperCase()}</span>
        <span className="viewer-lines">{lines.length} lines</span>
      </div>
      <div className="code-content">
        <table className="code-table">
          <tbody>
            {lines.map((line, i) => (
              <tr key={i} className="code-line">
                <td className="line-num">{i + 1}</td>
                <td className="line-code"><pre>{line}</pre></td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

function MarkdownRenderer({ content }) {
  const renderLine = (line, idx) => {
    if (line.startsWith('# ')) return <h1 key={idx}>{line.slice(2)}</h1>;
    if (line.startsWith('## ')) return <h2 key={idx}>{line.slice(3)}</h2>;
    if (line.startsWith('### ')) return <h3 key={idx}>{line.slice(4)}</h3>;
    if (line.startsWith('---')) return <hr key={idx} />;
    if (line.startsWith('- ')) return <li key={idx}>{renderInline(line.slice(2))}</li>;
    if (line.startsWith('| ')) {
      return <div key={idx} className="md-table-row"><pre>{line}</pre></div>;
    }
    if (line.trim() === '') return <br key={idx} />;
    return <p key={idx}>{renderInline(line)}</p>;
  };

  const renderInline = (text) => {
    const parts = [];
    let remaining = text;
    let key = 0;
    while (remaining.length > 0) {
      const boldMatch = remaining.match(/\*\*(.+?)\*\*/);
      const codeMatch = remaining.match(/`(.+?)`/);
      let first = null;
      let firstIdx = remaining.length;
      if (boldMatch && boldMatch.index < firstIdx) { first = 'bold'; firstIdx = boldMatch.index; }
      if (codeMatch && codeMatch.index < firstIdx) { first = 'code'; firstIdx = codeMatch.index; }
      if (!first) { parts.push(remaining); break; }
      if (firstIdx > 0) parts.push(remaining.slice(0, firstIdx));
      if (first === 'bold') {
        parts.push(<strong key={key++}>{boldMatch[1]}</strong>);
        remaining = remaining.slice(firstIdx + boldMatch[0].length);
      } else {
        parts.push(<code key={key++}>{codeMatch[1]}</code>);
        remaining = remaining.slice(firstIdx + codeMatch[0].length);
      }
    }
    return parts;
  };

  const lines = content.split('\n');
  const elements = [];
  let inCodeBlock = false;
  let codeLines = [];
  let codeLang = '';

  for (let i = 0; i < lines.length; i++) {
    if (lines[i].startsWith('```') && !inCodeBlock) {
      inCodeBlock = true;
      codeLang = lines[i].slice(3).trim();
      codeLines = [];
      continue;
    }
    if (lines[i].startsWith('```') && inCodeBlock) {
      inCodeBlock = false;
      elements.push(
        <div key={i} className="md-code-block">
          {codeLang && <span className="code-lang">{codeLang}</span>}
          <pre><code>{codeLines.join('\n')}</code></pre>
        </div>
      );
      continue;
    }
    if (inCodeBlock) {
      codeLines.push(lines[i]);
      continue;
    }
    elements.push(renderLine(lines[i], i));
  }

  return <div className="md-rendered">{elements}</div>;
}

function StatsBar({ stats }) {
  if (!stats) return null;
  return (
    <div className="stats-bar" data-testid="stats-bar">
      <span>{stats.total_files} files</span>
      <span>{stats.total_lines.toLocaleString()} lines</span>
      <span>{(stats.total_bytes / 1024).toFixed(1)} KB total</span>
      <span>{stats.file_counts['.c'] || 0} .c + {stats.file_counts['.h'] || 0} .h</span>
      <span>{stats.file_counts['.md'] || 0} docs</span>
    </div>
  );
}

function App() {
  const [files, setFiles] = useState([]);
  const [selectedPath, setSelectedPath] = useState('');
  const [fileContent, setFileContent] = useState(null);
  const [stats, setStats] = useState(null);
  const [loading, setLoading] = useState(false);
  const [sidebarOpen, setSidebarOpen] = useState(true);

  useEffect(() => {
    fetch(`${API}/api/project/tree`).then(r => r.json()).then(d => setFiles(d.files || []));
    fetch(`${API}/api/project/stats`).then(r => r.json()).then(d => setStats(d));
  }, []);

  const loadFile = useCallback(async (path) => {
    setSelectedPath(path);
    setLoading(true);
    try {
      const r = await fetch(`${API}/api/project/file?path=${encodeURIComponent(path)}`);
      const d = await r.json();
      setFileContent(d);
    } catch (e) {
      setFileContent({ content: `Error loading file: ${e.message}`, lang: 'text', path });
    }
    setLoading(false);
  }, []);

  useEffect(() => {
    if (files.length > 0 && !selectedPath) {
      loadFile('README.md');
    }
  }, [files, selectedPath, loadFile]);

  return (
    <div className="app" data-testid="app-root">
      <header className="app-header" data-testid="app-header">
        <button
          className="sidebar-toggle"
          onClick={() => setSidebarOpen(!sidebarOpen)}
          data-testid="sidebar-toggle"
        >
          {sidebarOpen ? '\u2190' : '\u2192'}
        </button>
        <div className="header-title">
          <h1>MN1-AA Client</h1>
          <span className="header-subtitle">WinCE 6.0 / MIPS / Au1320</span>
        </div>
        <StatsBar stats={stats} />
      </header>

      <div className="app-body">
        {sidebarOpen && (
          <aside className="sidebar" data-testid="sidebar">
            <FileTree files={files} selectedPath={selectedPath} onSelect={loadFile} />
          </aside>
        )}
        <main className="main-content" data-testid="main-content">
          {loading ? (
            <div className="loading" data-testid="loading">Loading...</div>
          ) : (
            <CodeViewer
              content={fileContent?.content}
              lang={fileContent?.lang}
              path={fileContent?.path}
            />
          )}
        </main>
      </div>
    </div>
  );
}

export default App;
