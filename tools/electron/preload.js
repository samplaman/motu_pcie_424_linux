// SPDX-License-Identifier: GPL-2.0-or-later
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
  platform: process.platform,
  version: process.versions.electron,
  isElectron: true,
  sendAction: (action, data) => ipcRenderer.send('app-action', { action, data })
});
