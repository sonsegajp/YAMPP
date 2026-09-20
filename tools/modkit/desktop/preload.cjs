const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('workshop', Object.freeze({
  request: (path, body) => ipcRenderer.invoke('workshop:request', path, body),
  importTexture: (options) => ipcRenderer.invoke('workshop:import-texture',options),
  importStage: () => ipcRenderer.invoke('workshop:import-stage'),
  exportAsset: (path, name) => ipcRenderer.invoke('workshop:export', path, name),
  onCommand: (handler) => ipcRenderer.on('workshop:command', (_event, command) => handler(command)),
}));
