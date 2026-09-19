#!/usr/bin/env bash
set -e

export PATH="/usr/local/bin:/opt/homebrew/bin:$PATH"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo "[*] Building PS2 MechaCon Tool using ps2homebrew container..."

if [ -d "/Users/imac/ps2dev" ]; then
    echo "[*] Building with native PS2DEV toolchain..."
    export PS2DEV=/Users/imac/ps2dev
    export PS2SDK=$PS2DEV/ps2sdk
    export PATH=$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2SDK/bin:$PATH
    make clean all
elif command -v docker >/dev/null 2>&1; then
    docker run --rm -v "$DIR:/src" -w /src ghcr.io/ps2homebrew/ps2homebrew:main make clean all
elif command -v podman >/dev/null 2>&1; then
    podman run --rm -v "$DIR:/src" -w /src ghcr.io/ps2homebrew/ps2homebrew:main make clean all
else
    echo "[-] Error: Neither local ps2dev, docker nor podman is available."
    exit 1
fi

if [ -f "$DIR/ps2_mecha_tool.elf" ] && [ -f "$DIR/ps2_mecha_tool_stripped.elf" ]; then
    echo "[+] SUCCESS: Built ps2_mecha_tool.elf ($(wc -c < "$DIR/ps2_mecha_tool.elf" | tr -d ' ') bytes)"
    echo "[+] SUCCESS: Built ps2_mecha_tool_stripped.elf ($(wc -c < "$DIR/ps2_mecha_tool_stripped.elf" | tr -d ' ') bytes)"
else
    echo "[-] Build failed: ELF binaries not found."
    exit 1
fi
