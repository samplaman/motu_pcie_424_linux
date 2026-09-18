# MOTU CueMix FX Studio — Electron Desktop App

Standalone cross-platform desktop application wrapping the MOTU PCI-424 studio engine in a hardware-accelerated, dark-themed Electron shell.

Works on **macOS**, **Linux**, and **Windows**.

---

## Prerequisites

- **Node.js** (v18 or newer) & **npm**
- **Python 3** (standard installation; no extra pip packages required)

---

## Quick Start

### 1. Install dependencies
```bash
cd tools/electron
npm install
```

### 2. Run locally in development / live mode
```bash
# Launches the app, automatically connects to PCI card or local driver:
npm start

# Or preview with synthetic demo hardware (24I/O + 1224 rig):
npm run start:demo
```

---

## Remote Studio Rig Control

You can run the audio engine on your Linux audio workstation (with the PCI-424 card installed), and control the CueMix mixer from your **Mac** or **Windows** machine over the local network:

```bash
# On your Mac / laptop:
MOTU_HOST=192.168.1.100 MOTU_PORT=8424 MOTU_REMOTE=1 npm start
```
*(Or use `File -> Connect to Remote Rig...` directly from inside the app!)*

---

## Building Native Installers

Use `electron-builder` to produce native distributables:

### macOS (`.dmg` & `.zip`)
```bash
npm run dist:mac
```
Produces `dist/MOTU CueMix FX Studio-2.0.0.dmg`.

### Linux (`.AppImage` & `.deb`)
```bash
npm run dist:linux
```
Produces `dist/MOTU CueMix FX Studio-2.0.0.AppImage` and `.deb`.

### Windows (`.exe` NSIS installer & portable)
```bash
npm run dist:win
```
Produces `dist/MOTU CueMix FX Studio Setup 2.0.0.exe`.
