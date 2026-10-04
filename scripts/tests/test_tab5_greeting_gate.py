"""Presence greeting fires only for someone who stays, once per visit."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5"

SMOKE = r'''
#include <cassert>
#include <cstdio>
#include "nabo_greeting_gate.h"
using nabo_vision::GreetingGate;
constexpr int64_t S = 1000000;
// Scan every second from t0 for n seconds with the given detection pattern.
static int Run(GreetingGate& g, int64_t t0, int n, bool person, int64_t& last_greeting) {
    int greets = 0;
    for (int i = 0; i < n; ++i) {
        const int64_t now = t0 + i * S;
        if (g.Scan(now, person, last_greeting)) { ++greets; last_greeting = now; }
    }
    return greets;
}
int main() {
    {   // Passer-by: seen for 3 seconds, never greeted.
        GreetingGate g; int64_t last = 0;
        assert(Run(g, 10 * S, 3, true, last) == 0);
        assert(Run(g, 13 * S, 30, false, last) == 0);
        // Two passers-by 10s apart still never accumulate a dwell.
        assert(Run(g, 43 * S, 3, true, last) == 0);
    }
    {   // Seated person: greeted once after ~6s, never again while present,
        // even across detector misses shorter and longer than the visit gap.
        GreetingGate g; int64_t last = 0;
        assert(Run(g, 10 * S, 6, true, last) == 0);
        assert(Run(g, 16 * S, 1, true, last) == 1);
        assert(Run(g, 17 * S, 600, true, last) == 0);
        assert(Run(g, 617 * S, 10, false, last) == 0);   // 10s miss
        assert(Run(g, 627 * S, 1200, true, last) == 0);  // still there 20 minutes
    }
    {   // Real return after a long absence: greeted again.
        GreetingGate g; int64_t last = 0;
        assert(Run(g, 10 * S, 8, true, last) == 1);
        assert(Run(g, 20 * S, 30, true, last) == 0);
        // Leaves for 15 minutes, comes back and sits down.
        assert(Run(g, 50 * S + 15 * 60 * S, 10, true, last) == 1);
    }
    {   // Short absence (2 min) inside the cooldown: no repeat.
        GreetingGate g; int64_t last = 0;
        assert(Run(g, 10 * S, 8, true, last) == 1);
        assert(Run(g, 18 * S + 120 * S, 10, true, last) == 0);
    }
    {   // Evaluating only while a visit is being measured.
        GreetingGate g; int64_t last = 0;
        assert(!g.Evaluating(0));
        g.Scan(5 * S, true, last);
        assert(g.Evaluating(6 * S) && !g.Evaluating(10 * S));
    }
    std::puts("PASS passers-by ignored; one greeting per stay; repeat only after a real return");
}
'''


class Tab5GreetingGateTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_dwell_and_once_per_visit(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "gate.cc"
            cpp.write_text(SMOKE)
            binary = Path(directory) / "gate"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
