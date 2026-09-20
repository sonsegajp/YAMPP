#pragma once
#include <atomic>

/* The network thread acquires the exact displayed aspect in one operation.
 * Settings changes race against that acquisition without changing a bound
 * value. No network thread touches the UI's non-atomic Settings structure. */
class AspectPreference {
  std::atomic<unsigned> state{0}; // bit 0: widescreen; bit 1: Online lock
public:
  void initialize(bool wide) { state.store(wide ? 1u : 0u); }
  int value() const { return int(state.load() & 1u); }
  bool locked() const { return (state.load() & 2u) != 0; }
  bool set(bool wide) {
    unsigned previous = state.load();
    for (;;) {
      if (previous & 2u) return false;
      if (state.compare_exchange_weak(previous, wide ? 1u : 0u)) return true;
    }
  }
  int acquire() { return int(state.fetch_or(2u) & 1u); }
  void release() { state.fetch_and(1u); }
};
