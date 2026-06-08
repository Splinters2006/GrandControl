#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

mkdir -p build/windows build/linux

echo "==> Agent (Windows release)..."
x86_64-w64-mingw32-g++ agent_relay.cpp -std=c++17 \
  -lws2_32 -lgdi32 -mwindows -static \
  -o build/windows/GrandControlAgent.exe

echo "==> Client (Linux)..."
g++ client_relay.cpp -std=c++17 \
  -o build/linux/grandcontrol-client

echo ""
echo "All builds complete."
echo "  build/windows/GrandControlAgent.exe"
echo "  build/linux/grandcontrol-client"
