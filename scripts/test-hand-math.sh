#!/usr/bin/env bash
#
# Checks the arm solver used for tracked controllers.
#
# The solver decides where Mario's arms go when his hands are pinned to PSVR2
# controllers, and none of it can be tried in the simulator - the controllers
# do not exist there. But the maths is plain C over float arrays, so it builds
# for the host and can be checked against forward kinematics through the very
# same Euler convention the renderer uses. That covers every sign and every
# half-turn without a headset.
#
# Usage: scripts/test-hand-math.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COOPDX="$REPO_ROOT/sm64coopdx"
WORK="$REPO_ROOT/.build/hand-math-test"

rm -rf "$WORK"
mkdir -p "$WORK"

echo "==> Building the solver for the host"
clang -O1 -g -std=gnu11 \
    -I"$COOPDX/src/pc/visionos" \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$WORK/hand_math_test" \
    "$REPO_ROOT/tests/hand_math_test.c" \
    "$COOPDX/src/pc/visionos/mariovision_hands_math.c" \
    -lm

echo "==> Running"
"$WORK/hand_math_test"
