"""
MN1 Android Auto Client - Documentation Server
Serves the project files and documentation via FastAPI.
"""

import os
from pathlib import Path
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware

app = FastAPI(title="MN1 Android Auto Client - Documentation Browser")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

PROJECT_ROOT = Path("/app/medianav-aa-client")

@app.get("/api/health")
async def health():
    return {"status": "healthy", "project": "MN1 Android Auto Client"}

@app.get("/api/project/tree")
async def get_project_tree():
    """Return the complete project file tree."""
    tree = []
    for root, dirs, files in os.walk(PROJECT_ROOT):
        dirs[:] = [d for d in sorted(dirs) if not d.startswith('.')]
        rel_root = os.path.relpath(root, PROJECT_ROOT)
        if rel_root == '.':
            rel_root = ''
        for f in sorted(files):
            if f.startswith('.'):
                continue
            rel_path = os.path.join(rel_root, f) if rel_root else f
            full_path = os.path.join(root, f)
            ext = os.path.splitext(f)[1].lower()
            lang = {
                '.c': 'c', '.h': 'c', '.md': 'markdown',
                '.py': 'python', '.java': 'java',
            }.get(ext, 'text')
            tree.append({
                "path": rel_path.replace('\\', '/'),
                "name": f,
                "size": os.path.getsize(full_path),
                "lang": lang,
                "is_doc": ext == '.md',
                "is_source": ext in ('.c', '.h'),
            })
    return {"files": tree, "total_files": len(tree)}

@app.get("/api/project/file")
async def get_file_content(path: str):
    """Return the content of a specific file."""
    safe_path = Path(PROJECT_ROOT) / path
    try:
        safe_path = safe_path.resolve()
        if not str(safe_path).startswith(str(PROJECT_ROOT.resolve())):
            raise HTTPException(status_code=403, detail="Access denied")
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid path")

    if not safe_path.exists():
        raise HTTPException(status_code=404, detail="File not found")

    try:
        content = safe_path.read_text(encoding='utf-8')
    except UnicodeDecodeError:
        content = safe_path.read_text(encoding='latin-1')

    ext = safe_path.suffix.lower()
    lang = {'.c': 'c', '.h': 'c', '.md': 'markdown', '.py': 'python'}.get(ext, 'text')

    return {
        "path": path,
        "content": content,
        "lang": lang,
        "size": len(content),
        "lines": content.count('\n') + 1,
    }

@app.get("/api/project/stats")
async def get_project_stats():
    """Return project statistics."""
    total_lines = 0
    total_bytes = 0
    file_counts = {}

    for root, dirs, files in os.walk(PROJECT_ROOT):
        dirs[:] = [d for d in dirs if not d.startswith('.')]
        for f in files:
            full_path = os.path.join(root, f)
            ext = os.path.splitext(f)[1].lower()
            file_counts[ext] = file_counts.get(ext, 0) + 1
            size = os.path.getsize(full_path)
            total_bytes += size
            try:
                with open(full_path, 'r', encoding='utf-8') as fh:
                    total_lines += sum(1 for _ in fh)
            except (UnicodeDecodeError, IOError):
                pass

    return {
        "total_lines": total_lines,
        "total_bytes": total_bytes,
        "file_counts": file_counts,
        "total_files": sum(file_counts.values()),
    }
