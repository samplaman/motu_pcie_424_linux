// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * MOTU CueMix FX Studio - Electron Desktop Shell
 * ===============================================
 * Spawns the high-performance Python studio engine and wraps it in a
 * native, hardware-accelerated desktop window for macOS, Linux, and Windows.
 */

const { app, BrowserWindow, Menu, shell, dialog } = require('electron');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

let mainWindow = null;
let pyProcess = null;

const HOST = process.env.MOTU_HOST || '127.0.0.1';
const PORT = parseInt(process.env.MOTU_PORT || '8424', 10);
const IS_REMOTE = Boolean(process.env.MOTU_REMOTE);
const IS_DEMO = Boolean(process.env.MOTU_DEMO) || process.argv.includes('--demo');

function waitForServer(url, timeoutMs = 15000) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    function ping() {
      http.get(url, (res) => {
        if (res.statusCode === 200) return resolve();
        retry();
      }).on('error', () => {
        retry();
      });
    }
    function retry() {
      if (Date.now() - start > timeoutMs) {
        return reject(new Error(`Timeout waiting for studio engine at ${url}`));
      }
      setTimeout(ping, 250);
    }
    ping();
  });
}

function startBackend() {
  if (IS_REMOTE) {
    console.log(`[Electron] Remote mode enabled; connecting directly to http://${HOST}:${PORT}`);
    return Promise.resolve();
  }

  return new Promise((resolve, reject) => {
    const pyScript = path.resolve(__dirname, '..', 'motu424-gui');
    const args = [pyScript, '--no-browser', `--port=${PORT}`, `--host=${HOST}`];
    if (IS_DEMO) args.push('--demo');

    // Add any pass-through CLI args like --rig=...
    for (const a of process.argv.slice(2)) {
      if (a.startsWith('--rig=')) args.push(a);
    }

    console.log(`[Electron] Launching studio backend: python3 ${args.join(' ')}`);
    pyProcess = spawn('python3', args, { stdio: ['ignore', 'pipe', 'pipe'] });

    pyProcess.stdout.on('data', (d) => {
      process.stdout.write(`[Engine] ${d}`);
    });

    pyProcess.stderr.on('data', (d) => {
      process.stderr.write(`[Engine ERR] ${d}`);
    });

    pyProcess.on('exit', (code) => {
      console.log(`[Electron] Backend process exited with code ${code}`);
      pyProcess = null;
    });

    pyProcess.on('error', (err) => {
      console.error('[Electron] Failed to spawn Python backend:', err);
      reject(err);
    });

    // Wait for the HTTP endpoint to respond
    waitForServer(`http://${HOST}:${PORT}/api/status`)
      .then(resolve)
      .catch(reject);
  });
}

function createMainWindow() {
  mainWindow = new BrowserWindow({
    width: 1280,
    height: 840,
    minWidth: 980,
    minHeight: 640,
    backgroundColor: '#121316',
    title: 'MOTU CueMix FX Studio',
    titleBarStyle: process.platform === 'darwin' ? 'hiddenInset' : 'default',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: true
    }
  });

  const targetUrl = `http://${HOST}:${PORT}`;
  mainWindow.loadURL(targetUrl);

  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url);
    return { action: 'deny' };
  });

  mainWindow.on('closed', () => {
    mainWindow = null;
  });

  setupMenu();
}

function setupMenu() {
  const isMac = process.platform === 'darwin';
  const template = [
    ...(isMac ? [{
      label: app.name,
      submenu: [
        { role: 'about' },
        { type: 'separator' },
        { role: 'services' },
        { type: 'separator' },
        { role: 'hide' },
        { role: 'hideOthers' },
        { role: 'unhide' },
        { type: 'separator' },
        { role: 'quit' }
      ]
    }] : []),
    {
      label: 'File',
      submenu: [
        {
          label: 'Connect to Remote Rig...',
          accelerator: 'CmdOrCtrl+O',
          click: async () => {
            // Prompt for remote server address
            if (!mainWindow) return;
            mainWindow.webContents.executeJavaScript(`
              const remote = prompt("Enter Linux PCI-424 Rig address:", "http://192.168.1.100:8424");
              if (remote) window.location.href = remote;
            `);
          }
        },
        { type: 'separator' },
        isMac ? { role: 'close' } : { role: 'quit' }
      ]
    },
    {
      label: 'View',
      submenu: [
        { role: 'reload' },
        { role: 'forceReload' },
        { role: 'toggleDevTools' },
        { type: 'separator' },
        { role: 'resetZoom' },
        { role: 'zoomIn' },
        { role: 'zoomOut' },
        { type: 'separator' },
        { role: 'togglefullscreen' }
      ]
    },
    {
      label: 'Tabs',
      submenu: [
        {
          label: 'CueMix Console',
          accelerator: 'CmdOrCtrl+1',
          click: () => mainWindow && mainWindow.webContents.executeJavaScript('showTab("mixer")')
        },
        {
          label: 'DSP Studio',
          accelerator: 'CmdOrCtrl+2',
          click: () => mainWindow && mainWindow.webContents.executeJavaScript('showTab("dsp")')
        },
        {
          label: 'Routing Patchbay',
          accelerator: 'CmdOrCtrl+3',
          click: () => mainWindow && mainWindow.webContents.executeJavaScript('showTab("patchbay")')
        },
        {
          label: 'Hardware Workbench',
          accelerator: 'CmdOrCtrl+4',
          click: () => mainWindow && mainWindow.webContents.executeJavaScript('showTab("workbench")')
        }
      ]
    },
    {
      label: 'Help',
      submenu: [
        {
          label: 'GitHub Repository',
          click: () => shell.openExternal('https://github.com/samplaman/motu_pcie_424_linux')
        },
        {
          label: 'Documentation',
          click: () => shell.openExternal('https://github.com/samplaman/motu_pcie_424_linux#readme')
        }
      ]
    }
  ];

  const menu = Menu.buildFromTemplate(template);
  Menu.setApplicationMenu(menu);
}

function cleanup() {
  if (pyProcess) {
    console.log('[Electron] Terminating backend engine...');
    pyProcess.kill('SIGTERM');
    setTimeout(() => {
      if (pyProcess) pyProcess.kill('SIGKILL');
    }, 2000);
  }
}

app.whenReady().then(async () => {
  try {
    await startBackend();
    createMainWindow();
  } catch (err) {
    console.error('[Electron] Startup failure:', err);
    dialog.showErrorBox(
      'Startup Error',
      `Failed to launch MOTU Studio backend:\n${err.message}\n\nPlease verify python3 is available.`
    );
    app.quit();
  }

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createMainWindow();
  });
});

app.on('before-quit', cleanup);

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit();
  }
});
