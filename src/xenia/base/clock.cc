/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/clock.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/base/mutex.h"

#if defined(_WIN32)

#include "xenia/base/platform_win.h"

#endif

DEFINE_bool(clock_no_scaling, false,
            "Disable scaling code. Time management and locking is bypassed. "
            "Guest system time is directly pulled from host.",
            "CPU");
DEFINE_bool(clock_source_raw, false,
            "On x64, Use the RDTSC instruction as the time source. Requires "
            "invariant TSC. "
            "On a64, Use the CNTVCT_EL0 register as the time source",
            "CPU");

namespace xe {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();

std::pair<uint64_t, uint64_t> guest_system_time_ratio_ =
    []() -> std::pair<uint64_t, uint64_t> {
  std::pair<uint64_t, uint64_t> frac(uint64_t(10000000), guest_tick_frequency_);
  reduce_fraction(frac);
  return frac;
}();

// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Native guest ticks, the latest value returned by UpdateGuestClock. Exposed
// for inline timebase reads in generated code.
std::atomic<uint64_t> last_guest_tick_count_{0};
static_assert(std::atomic<uint64_t>::is_always_lock_free &&
                  sizeof(last_guest_tick_count_) == sizeof(uint64_t),
              "The guest tick count must be readable as a uint64_t");

// The guest tick count is computed as
// guest_base + (host_tick_count - host_base) * ratio.first / ratio.second,
// which only needs synchronization when the ratio changes. The parameters are
// changed with tick_mutex_ locked, and read without locking with a sequence
// lock, as the guest reads the timebase very frequently from many threads.
std::atomic<uint32_t> guest_clock_sequence_{0};
std::atomic<uint64_t> guest_clock_host_base_{Clock::QueryHostTickCount()};
std::atomic<uint64_t> guest_clock_guest_base_{0};
std::atomic<uint64_t> guest_clock_ratio_first_{1};
std::atomic<uint64_t> guest_clock_ratio_second_{1};

// value * numerator / denominator without overflowing in the intermediate
// product as long as the result and remainder * numerator fit.
static uint64_t MulDiv(uint64_t value, uint64_t numerator,
                       uint64_t denominator) {
  return value / denominator * numerator +
         value % denominator * numerator / denominator;
}

static uint64_t ComputeGuestTickCount(uint64_t host_tick_count) {
  uint64_t host_base, guest_base, ratio_first, ratio_second;
  uint32_t sequence;
  do {
    sequence = guest_clock_sequence_.load(std::memory_order_acquire);
    host_base = guest_clock_host_base_.load(std::memory_order_relaxed);
    guest_base = guest_clock_guest_base_.load(std::memory_order_relaxed);
    ratio_first = guest_clock_ratio_first_.load(std::memory_order_relaxed);
    ratio_second = guest_clock_ratio_second_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
  } while ((sequence & 1) ||
           sequence != guest_clock_sequence_.load(std::memory_order_relaxed));
  uint64_t host_delta =
      host_tick_count > host_base ? host_tick_count - host_base : 0;
  return guest_base + MulDiv(host_delta, ratio_first, ratio_second);
}

using tick_mutex_type = std::mutex;

// Mutex serializing changes of the guest clock parameters.
// std::mutex tick_mutex_;
static tick_mutex_type tick_mutex_;

static void RecomputeGuestSystemTimeRatio() {
  std::pair<uint64_t, uint64_t> frac(uint64_t(10000000), guest_tick_frequency_);
  reduce_fraction(frac);
  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  guest_system_time_ratio_ = frac;
}

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac =
      std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  guest_tick_ratio_ = frac;
  // Continue from the current guest tick count with the new ratio.
  uint64_t host_tick_count = Clock::QueryHostTickCount();
  uint64_t guest_tick_count = ComputeGuestTickCount(host_tick_count);
  uint32_t sequence = guest_clock_sequence_.load(std::memory_order_relaxed);
  guest_clock_sequence_.store(sequence + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  guest_clock_host_base_.store(host_tick_count, std::memory_order_relaxed);
  guest_clock_guest_base_.store(guest_tick_count, std::memory_order_relaxed);
  guest_clock_ratio_first_.store(frac.first, std::memory_order_relaxed);
  guest_clock_ratio_second_.store(frac.second, std::memory_order_relaxed);
  guest_clock_sequence_.store(sequence + 2, std::memory_order_release);
}

// Update the guest timer for all threads.
// Return a copy of the value so locking is reduced.
uint64_t UpdateGuestClock() {
  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (cvars::clock_no_scaling) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  uint64_t guest_tick_count = ComputeGuestTickCount(host_tick_count);
  // Publish the value for inline reads, without ever going backwards when
  // multiple threads race.
  uint64_t last = last_guest_tick_count_.load(std::memory_order_relaxed);
  while (last < guest_tick_count &&
         !last_guest_tick_count_.compare_exchange_weak(
             last, guest_tick_count, std::memory_order_relaxed)) {
  }
  return std::max(last, guest_tick_count);
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (cvars::clock_no_scaling) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  return guest_tick_count * guest_system_time_ratio_.first /
         guest_system_time_ratio_.second;
}
uint64_t Clock::QueryHostTickFrequency() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_source_raw) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_source_raw) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() { return guest_time_scalar_; }

void Clock::set_guest_time_scalar(double scalar) {
  if (cvars::clock_no_scaling) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() { return guest_tick_frequency_; }

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestSystemTimeRatio();
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() { return guest_system_time_base_; }

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t* Clock::GetGuestTickCountPointer() {
  // Only read by generated code, which doesn't use std::atomic.
  return reinterpret_cast<uint64_t*>(&last_guest_tick_count_);
}
uint64_t Clock::QueryGuestSystemTime() {
  if (cvars::clock_no_scaling) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint64_t Clock::QueryGuestInterruptTime() {
  return Clock::QueryHostInterruptTime();
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(
      std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                         std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (cvars::clock_no_scaling) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (cvars::clock_no_scaling) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms = static_cast<uint64_t>(
      (static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (cvars::clock_no_scaling) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time =
        static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time = static_cast<uint64_t>(
        (static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (cvars::clock_no_scaling) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) *
                                              guest_time_scalar_);
  uint64_t scaled_usec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) *
                                               guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace xe
