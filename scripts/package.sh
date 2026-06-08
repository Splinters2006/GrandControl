#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ ! -f build/windows/GrandControlAgent.exe ]; then
  echo "ERROR: build/windows/GrandControlAgent.exe not found. Run 'Build: ALL platforms' first."
  exit 1
fi

mkdir -p dist

cp build/windows/GrandControlAgent.exe installer/
cd installer
zip -j "$ROOT/dist/GrandControl-windows.zip" \
  GrandControlAgent.exe \
  install_agent.ps1 \
  install_agent.bat
rm GrandControlAgent.exe
cd "$ROOT"

echo "Packaged: dist/GrandControl-windows.zip"
echo "Upload GrandControlAgent.exe from build/windows/ as a GitHub Release asset."
