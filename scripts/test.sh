#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT
CXX="${CXX:-g++}"

# Prevent Windows CRLF line endings from breaking scripts under WSL/Linux.
for script in "$ROOT"/scripts/*.sh "$ROOT"/tier4/applet-mitm/build.sh; do
    bash -n "$script"
    if LC_ALL=C grep -q "$(printf '\r')" "$script"; then
        echo "ERROR: CRLF line endings in $script" >&2
        exit 1
    fi
done

"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tests/settings_test.cpp" -o "$TEST_DIR/settings"
"$TEST_DIR/settings"
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tier4/applet-mitm/test/ambilight_test.cpp" -o "$TEST_DIR/ambilight"
"$TEST_DIR/ambilight"
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -I "$ROOT/tier4/applet-mitm/source" \
    "$ROOT/tier4/applet-mitm/test/armfile_test.cpp" -o "$TEST_DIR/armfile"
"$TEST_DIR/armfile"
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tests/config_migration_test.cpp" -o "$TEST_DIR/config_migration"
mkdir "$TEST_DIR/migration"
(cd "$TEST_DIR/migration" && "$TEST_DIR/config_migration")
