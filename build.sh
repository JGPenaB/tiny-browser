#!/usr/bin/env bash
# Quiet build for Git Bash: prints only errors, warnings and the result line.
cd "$(dirname "$0")"
cmake --build build --config Release -- -nologo -verbosity:minimal 2>&1 | grep -E "error|warning|-> " | sed 's/\[E:.*vcxproj\]//' | head -${1:-40}
