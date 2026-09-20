#include "../../aurora_shim/aspect_preference.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <initializer_list>

int main() {
  AspectPreference aspect;
  for (bool initial : {false, true}) {
    aspect.initialize(initial);
    assert(aspect.acquire() == int(initial));
    assert(aspect.locked() && !aspect.set(!initial));
    assert(aspect.value() == int(initial));
    aspect.release(); assert(!aspect.locked());
    assert(aspect.set(!initial) && aspect.value() == int(!initial));
  }
  /* UI edits racing a network acquisition must never alter its bound mode. */
  std::atomic<bool> stop{false};
  std::thread editor([&] { while (!stop.load()) { aspect.set(false); aspect.set(true); } });
  for (int round = 0; round < 5000; ++round) {
    int bound = aspect.acquire();
    assert(aspect.locked());
    for (int check = 0; check < 8; ++check) {
      assert(!aspect.set(!bound));
      assert(aspect.value() == bound);
    }
    aspect.release();
  }
  stop.store(true); editor.join();
  assert(!aspect.locked());
  puts("PASS: offline aspect preferences, rejected locked edits, and 5000 concurrent acquire/edit races");
}
