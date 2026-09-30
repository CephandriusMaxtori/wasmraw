// Headless browser check for the WasmRaw editor.
//
// The C++ smoke test drives the decoder module directly and the shell harness
// runs app/shell.html against a mocked Module, so neither of them exercises a
// real browser: the GL context, the worker, the C++/JS heap bridge, or ImGui's
// own runtime assertions. That gap let two real bugs through - a PushFont/PopFont
// ordering mistake in the menu bar and a missing heap export that only threw
// once a subject mask existed. Both produced console errors and neither test
// noticed, so this script asserts on the console instead.
//
// Usage: node tests/browser_check.mjs [build-dir] [fixture-dng]

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { extname, join, resolve } from 'node:path';
import { chromium } from 'playwright';

const buildDir = resolve(process.argv[2] || 'build-wasm');
const fixture = resolve(process.argv[3] || 'smoke/test.dng');
const shotDir = resolve('artifacts');
const port = 8123;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.dng': 'image/x-raw'
};

const failures = [];
const notes = [];

function record(ok, message) {
  (ok ? notes : failures).push(`${ok ? 'PASS' : 'FAIL'}  ${message}`);
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${message}`);
}

function startServer() {
  const server = createServer(async (request, response) => {
    const path = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
    const file = join(buildDir, path === '/' ? '/wasmraw.html' : path);
    if (!file.startsWith(buildDir) || !existsSync(file)) {
      response.writeHead(404).end('not found');
      return;
    }
    try {
      const body = await readFile(file);
      response.writeHead(200, {
        'Content-Type': MIME[extname(file).toLowerCase()] || 'application/octet-stream',
        'Cache-Control': 'no-store'
      });
      response.end(body);
    } catch (error) {
      response.writeHead(500).end(String(error));
    }
  });
  return new Promise((resolvePromise) => {
    server.listen(port, () => resolvePromise(server));
  });
}

async function main() {
  if (!existsSync(join(buildDir, 'wasmraw.html'))) {
    throw new Error(`no wasmraw.html in ${buildDir}; build first`);
  }
  if (!existsSync(fixture)) {
    throw new Error(`missing fixture ${fixture}; run smoke/make_test_dng.py first`);
  }

  mkdirSync(shotDir, { recursive: true });
  const server = await startServer();
  const rawBytes = await readFile(fixture);

  // Software GL so WebGL2 works on a headless runner with no GPU.
  const browser = await chromium.launch({
    args: [
      '--use-gl=angle',
      '--use-angle=swiftshader',
      '--enable-unsafe-swiftshader',
      '--no-sandbox'
    ]
  });

  const consoleLines = [];
  const pageErrors = [];
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    page.on('console', (message) => consoleLines.push(`[${message.type()}] ${message.text()}`));
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    await page.goto(`http://localhost:${port}/wasmraw.html`, { waitUntil: 'domcontentloaded' });

    // The runtime must come up, and WebGL2 must exist or nothing else matters.
    await page.waitForFunction(() => window._wasmraw && window._wasmraw.ready, null,
      { timeout: 60000 });
    record(true, 'runtime initialized');

    const webgl2 = await page.evaluate(() =>
      !!document.createElement('canvas').getContext('webgl2'));
    record(webgl2, 'WebGL2 available');

    // Feed the synthetic DNG through the same entry point the UI uses.
    await page.evaluate((base64) => {
      const binary = atob(base64);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
      return window._wasmraw.decode(bytes, 'test.dng');
    }, rawBytes.toString('base64'));
    record(true, 'decode request accepted');

    // The shell hides the splash once the active image has decoded, which only
    // happens after the worker round-trip and the first preview render.
    await page.waitForFunction(
      () => document.getElementById('splash').classList.contains('hide'),
      null, { timeout: 90000 });
    record(true, 'image decoded and splash hidden');

    // Exercise the settings path: theme swap and a font-size change both rebuild
    // the font atlas between frames, which is where ImGui misuse shows up.
    await page.evaluate(() => window.Module._wasm_apply_ui_settings(1, 15, 16, 1));
    await page.waitForTimeout(600);
    record(true, 'settings applied (theme + sizes + fps)');

    await page.waitForTimeout(400);
    await page.screenshot({ path: join(shotDir, 'browser-check.png') });

    // ImGui reports contract violations through the console, not by throwing.
    const imguiErrors = consoleLines.filter((line) => line.includes('[imgui-error]'));
    record(imguiErrors.length === 0,
      `no ImGui runtime errors (${imguiErrors.length} seen)`);
    if (imguiErrors.length) notes.push(...imguiErrors.slice(0, 10).map((line) => `    ${line}`));

    const errorConsole = consoleLines.filter((line) => line.startsWith('[error]'));
    record(errorConsole.length === 0, `no console errors (${errorConsole.length} seen)`);
    if (errorConsole.length) notes.push(...errorConsole.slice(0, 10).map((line) => `    ${line}`));

    record(pageErrors.length === 0, `no uncaught page errors (${pageErrors.length} seen)`);
    if (pageErrors.length) notes.push(...pageErrors.slice(0, 10).map((line) => `    ${line}`));
  } finally {
    writeFileSync(join(shotDir, 'browser-console.log'), consoleLines.join('\n'), 'utf8');
    writeFileSync(join(shotDir, 'browser-page-errors.log'), pageErrors.join('\n'), 'utf8');
    await browser.close();
    server.close();
  }

  console.log('');
  for (const line of notes) console.log(line);
  if (failures.length) {
    console.log('');
    console.log(`${failures.length} browser check(s) failed`);
    process.exit(1);
  }
  console.log('browser check passed');
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
