#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Catch CRLF regressions before the Docker/ARM64 build.
if LC_ALL=C grep -q 
CXX="${CXX:-g++}"
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tests/settings_test.cpp" -o /tmp/switch-ambilight-tests/settings
/tmp/switch-ambilight-tests/settings
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tier4/applet-mitm/test/ambilight_test.cpp" -o /tmp/switch-ambilight-tests/ambilight
/tmp/switch-ambilight-tests/ambilight
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -I "$ROOT/tier4/applet-mitm/source" \
 "$ROOT/tier4/applet-mitm/test/armfile_test.cpp" -o /tmp/switch-ambilight-tests/armfile
/tmp/switch-ambilight-tests/armfile

"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tests/config_migration_test.cpp" -o /tmp/switch-ambilight-tests/config_migration
TMP_CFG_DIR="$(mktemp -d)"
(cd "$TMP_CFG_DIR" && /tmp/switch-ambilight-tests/config_migration)
rm -rf "$TMP_CFG_DIR"
\r' "$ROOT/tier4/applet-mitm/build.sh"; then
  echo "ERROR: tier4/applet-mitm/build.sh contains CRLF line endings" >&2
  exit 1
fi
bash -n "$ROOT/tier4/applet-mitm/build.sh"
mkdir -p /tmp/switch-ambilight-tests
CXX="${CXX:-g++}"
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tests/settings_test.cpp" -o /tmp/switch-ambilight-tests/settings
/tmp/switch-ambilight-tests/settings
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tier4/applet-mitm/test/ambilight_test.cpp" -o /tmp/switch-ambilight-tests/ambilight
/tmp/switch-ambilight-tests/ambilight
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -I "$ROOT/tier4/applet-mitm/source" \
 "$ROOT/tier4/applet-mitm/test/armfile_test.cpp" -o /tmp/switch-ambilight-tests/armfile
/tmp/switch-ambilight-tests/armfile

"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror "$ROOT/tests/config_migration_test.cpp" -o /tmp/switch-ambilight-tests/config_migration
TMP_CFG_DIR="$(mktemp -d)"
(cd "$TMP_CFG_DIR" && /tmp/switch-ambilight-tests/config_migration)
rm -rf "$TMP_CFG_DIR"
