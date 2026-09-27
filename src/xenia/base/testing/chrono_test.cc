/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/chrono.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "xenia/base/clock.h"

#define CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER
#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

// If the tests run fast, the time duration may be zero. So we artificially
// raise it to allow for some error.
template <typename _Rep, typename _Period>
auto dur_bound(std::chrono::duration<_Rep, _Period> dur) {
  using namespace std::chrono_literals;
  return std::max<std::common_type_t<std::chrono::milliseconds,
                                     std::chrono::duration<_Rep, _Period>>>(
      dur, 1ms);
}

TEST_CASE("WinSystemClock <-> system_clock", "[clock_cast]") {
  using namespace xe::chrono;
  using namespace date;
  using sys_clock = std::chrono::system_clock;

  // First check some assumptions made in chrono.h

  SECTION("C++20 unix epoch") {
    auto epoch = date::year{1970} / date::month{1} / date::day{1};
    std::chrono::system_clock::time_point epoch_tp{
        static_cast<date::sys_days>(epoch)};
    REQUIRE(epoch_tp.time_since_epoch().count() == 0);
  }

  SECTION("Epoch delta") {
    std::chrono::seconds sys_delta = WinSystemClock::unix_epoch_delta();
    REQUIRE(sys_delta.count() == INT64_C(-11644473600));
  }

  SECTION("1993/12/21") {
    static constexpr sys_days sys(1993_y / dec / 21);

    static constexpr auto wsys = WinSystemClock::from_sys(sys);
    REQUIRE(wsys.time_since_epoch().count() == 124009056000000000);

    REQUIRE(clock_cast<sys_clock>(wsys) == sys);
    REQUIRE(clock_cast<WinSystemClock>(sys) == wsys);
  }

  SECTION("2100/3/1") {
    static constexpr sys_days sys(2100_y / mar / 1);

    static constexpr auto wsys = WinSystemClock::from_sys(sys);
    REQUIRE(wsys.time_since_epoch().count() == 157520160000000000);

    REQUIRE(clock_cast<sys_clock>(wsys) == sys);
    REQUIRE(clock_cast<WinSystemClock>(sys) == wsys);
  }
}

TEST_CASE("WinSystemClock <-> XSystemClock", "[clock_cast]") {
  using namespace std::chrono_literals;
  using namespace xe::chrono;
  using namespace date;
  using sys_clock = std::chrono::system_clock;

  SECTION("1993/12/21, clock_no_scaling = true") {
    static constexpr sys_days sys(1993_y / dec / 21);
    cvars::clock_no_scaling = true;
    Clock::set_guest_time_scalar(1.0);

    static constexpr auto wsys = WinSystemClock::from_sys(sys);

    auto start = std::chrono::system_clock::now();

    auto xsys = date::clock_cast<XSystemClock>(wsys);
    auto wxsys = date::clock_cast<WinSystemClock>(xsys);

    auto duration = dur_bound(std::chrono::system_clock::now() - start);

    auto error = wsys.time_since_epoch() - wxsys.time_since_epoch();
    REQUIRE(error < duration);
    REQUIRE(error > -duration);
  }

  SECTION("1993/12/21, clock_no_scaling = false") {
    static constexpr sys_days sys(1993_y / dec / 21);
    cvars::clock_no_scaling = false;
    Clock::set_guest_time_scalar(1.0);

    static constexpr auto wsys = WinSystemClock::from_sys(sys);

    auto start = std::chrono::system_clock::now();

    auto xsys = date::clock_cast<XSystemClock>(wsys);
    auto wxsys = date::clock_cast<WinSystemClock>(xsys);

    auto duration = dur_bound(std::chrono::system_clock::now() - start);

    auto error1 = wsys.time_since_epoch() - xsys.time_since_epoch();
    auto error2 = xsys.time_since_epoch() - wxsys.time_since_epoch();
    auto error3 = wsys - wxsys;

    // In AppVeyor, the difference often can be as large as roughly 16ms.
    REQUIRE(error1 < 20ms);
    REQUIRE(error1 > -20ms);
    REQUIRE(error2 < 20ms);
    REQUIRE(error2 > -20ms);
    REQUIRE(error3 < duration);
    REQUIRE(error3 > -duration);
  }
}

}  // namespace xe::base::test

// only make these available now
#include "xenia/base/chrono_steady_cast.h"

namespace xe::base::test {

TEST_CASE("WinSystemClock <-> steady_clock", "[clock_cast]") {
  using namespace xe::chrono;
  using namespace date;
  using sty_clock = std::chrono::steady_clock;

  // steady conversion is mostly used to convert wait times

  SECTION("now") {
    auto sty = sty_clock::now();

    // Because steady casts are imprecise, we need to allow some margin of error
    auto start = sty_clock::now();

    auto wsty = clock_cast<WinSystemClock>(sty);
    auto sty2 = clock_cast<sty_clock>(wsty);

    auto duration = dur_bound(sty_clock::now() - start).count();
    auto error = std::abs((sty2 - sty).count());
    REQUIRE(error <= duration);
  }
}

TEST_CASE("Guest tick count", "[clock]") {
  using namespace std::chrono_literals;
  Clock::set_guest_tick_frequency(50000000);
  Clock::set_guest_time_scalar(1.0);

  SECTION("advances at the guest tick frequency") {
    uint64_t start_ticks = Clock::QueryGuestTickCount();
    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(100ms);
    uint64_t end_ticks = Clock::QueryGuestTickCount();
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    double expected_ticks = seconds * 50000000.0;
    REQUIRE(end_ticks > start_ticks);
    // Allow for the time between the clock reads and the steady clock reads.
    REQUIRE(std::abs(double(end_ticks - start_ticks) - expected_ticks) <
            expected_ticks * 0.1);
  }

  SECTION("never goes backwards across threads and scalar changes") {
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&failed] {
        uint64_t last = 0;
        for (int j = 0; j < 20000; ++j) {
          uint64_t ticks = Clock::QueryGuestTickCount();
          if (ticks < last) {
            failed = true;
          }
          last = ticks;
        }
      });
    }
    Clock::set_guest_time_scalar(2.0);
    Clock::set_guest_time_scalar(1.0);
    for (auto& thread : threads) {
      thread.join();
    }
    REQUIRE_FALSE(failed);
  }
}

}  // namespace xe::base::test
