#!/bin/bash
# Alphastudio SAT 76 - Instalator macOS
# Wersja: 1.1.0

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"

echo ""
echo "  Alphastudio SAT 76 - Instalator macOS"
echo "  ======================================"
echo ""

mkdir -p "$VST3_DIR"
mkdir -p "$AU_DIR"

# Instalacja VST3
VST3_FILE=$(find "$SCRIPT_DIR" -maxdepth 1 -name "*.vst3" -type d | head -1)
if [ -n "$VST3_FILE" ]; then
  VST3_NAME=$(basename "$VST3_FILE")
  echo "  -> Instaluje VST3: $VST3_NAME"
  rm -rf "$VST3_DIR/$VST3_NAME"
  cp -R "$VST3_FILE" "$VST3_DIR/"
  xattr -rd com.apple.quarantine "$VST3_DIR/$VST3_NAME" 2>/dev/null || true
  echo "     OK: $VST3_DIR/$VST3_NAME"
else
  echo "  Nie znaleziono pliku .vst3"
fi

# Instalacja AU
AU_FILE=$(find "$SCRIPT_DIR" -maxdepth 1 -name "*.component" -type d | head -1)
if [ -n "$AU_FILE" ]; then
  AU_NAME=$(basename "$AU_FILE")
  echo "  -> Instaluje AU: $AU_NAME"
  rm -rf "$AU_DIR/$AU_NAME"
  cp -R "$AU_FILE" "$AU_DIR/"
  xattr -rd com.apple.quarantine "$AU_DIR/$AU_NAME" 2>/dev/null || true
  echo "     OK: $AU_DIR/$AU_NAME"
fi

echo ""
echo "  Instalacja zakonczona!"
echo "  Uruchom ponownie DAW i wykonaj skanowanie wtyczek."
echo ""
