#!/usr/bin/env node
// Minimal static server for previewing micropatterns_emulator locally.
// Node rather than `python3 -m http.server` because that module calls
// os.getcwd() at import time, which this sandbox refuses.
//
//   node tools/dev/serve.mjs [port] [root]
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { join, extname, normalize } from 'node:path';

const port = Number(process.argv[2] || 8777);
const root = process.argv[3] || 'micropatterns_emulator';

const TYPES = {
    '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
    '.css': 'text/css', '.json': 'application/json', '.wasm': 'application/wasm',
    '.png': 'image/png', '.svg': 'image/svg+xml', '.mp': 'text/plain',
};

createServer(async (req, res) => {
    try {
        let p = decodeURIComponent(req.url.split('?')[0]);
        if (p.endsWith('/')) p += 'index.html';
        // normalize() collapses any ../ before it reaches the filesystem.
        const file = join(root, normalize(p).replace(/^(\.\.[/\\])+/, ''));
        await stat(file);
        const body = await readFile(file);
        res.writeHead(200, {
            'Content-Type': TYPES[extname(file)] || 'application/octet-stream',
            'Cache-Control': 'no-store',
        });
        res.end(body);
    } catch {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('not found');
    }
}).listen(port, () => console.log(`serving ${root} on http://localhost:${port}`));
