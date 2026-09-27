/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cmath>
#include <limits>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// =============================================================================
// FPCR isolation: VMX vector float ops must not leak their FPCR state into
// subsequent scalar FP operations.
//
// The bug: without scoped FPCR save/restore, a VMX op sets FPCR to
// round-to-nearest + flush-to-zero, and that state persists into the next
// scalar FP op — overriding whatever rounding mode the scalar path expects.
// =============================================================================

TEST_CASE("VMX_FPCR_DOES_NOT_LEAK_INTO_SCALAR", "[backend]") {
  // Strategy:
  //   1. SET_ROUNDING_MODE to toward-positive-infinity (mode 2)
  //   2. Do a VMX vector float add (internally sets FPCR to RN + FZ)
  //   3. Do a scalar float add of 1.0 + 2^-24
  //   4. If FPCR leaked, the scalar add uses round-to-nearest → result = 1.0
  //      If FPCR was properly restored, it uses toward-+inf → result > 1.0
  TestFunction test([](HIRBuilder& b) {
    // Set scalar rounding to toward +infinity.
    b.SetRoundingMode(b.LoadConstantInt32(2));

    // VMX vector float add — this touches FPCR internally.
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));

    // Now do a scalar float add. If FPCR leaked, this will round-to-nearest.
    auto a = b.Convert(LoadFPR(b, 6), FLOAT32_TYPE);
    auto c = b.Convert(LoadFPR(b, 7), FLOAT32_TYPE);
    auto sum = b.Add(a, c);
    StoreFPR(b, 3, b.Convert(sum, FLOAT64_TYPE));

    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // Vector inputs — just normal values, we don't care about the result.
        ctx->v[4] = vec128f(1.0f, 2.0f, 3.0f, 4.0f);
        ctx->v[5] = vec128f(5.0f, 6.0f, 7.0f, 8.0f);
        // Scalar inputs: 1.0 + 2^-24 — rounds differently under different
        // modes.
        ctx->f[6] = 1.0;
        ctx->f[7] = std::ldexp(1.0, -24);
      },
      [&test](PPCContext* ctx) {
        auto result = static_cast<float>(ctx->f[3]);
        // Under toward-+infinity, 1.0f + 2^-24 rounds UP to nextafter(1.0f).
        // Under round-to-nearest, it rounds to 1.0f (ties to even).
        float expected = std::nextafterf(1.0f, 2.0f);
        REQUIRE(result == expected);
        // Reset rounding mode for subsequent tests.
        test.processors[0]->backend()->SetGuestRoundingMode(ctx, 0);
      });
}

TEST_CASE("VMX_FPCR_DOES_NOT_LEAK_INTO_SCALAR_MULTIPLE_OPS", "[backend]") {
  // Same idea but with two consecutive VMX vector float adds before the
  // scalar op, to verify FPCR is restored after each one.
  TestFunction test([](HIRBuilder& b) {
    b.SetRoundingMode(b.LoadConstantInt32(2));  // toward +inf

    // Two VMX vector float adds back to back.
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    StoreVR(b, 6, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 3), FLOAT32_TYPE));

    // Scalar add — must still use toward-+inf.
    auto a = b.Convert(LoadFPR(b, 6), FLOAT32_TYPE);
    auto c = b.Convert(LoadFPR(b, 7), FLOAT32_TYPE);
    auto sum = b.Add(a, c);
    StoreFPR(b, 3, b.Convert(sum, FLOAT64_TYPE));

    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128f(1.0f, 2.0f, 3.0f, 4.0f);
        ctx->v[5] = vec128f(5.0f, 6.0f, 7.0f, 8.0f);
        ctx->f[6] = 1.0;
        ctx->f[7] = std::ldexp(1.0, -24);
      },
      [&test](PPCContext* ctx) {
        auto result = static_cast<float>(ctx->f[3]);
        float expected = std::nextafterf(1.0f, 2.0f);
        REQUIRE(result == expected);
        test.processors[0]->backend()->SetGuestRoundingMode(ctx, 0);
      });
}

TEST_CASE("VMX_FPCR_DOES_NOT_LEAK_INTO_SCALAR_ACROSS_BLOCKS", "[backend]") {
  // The FPCR mode isn't known statically at the start of a block. A scalar
  // block reached after a VMX op, either by a branch or by falling through
  // from another VMX block, must switch back to the guest FPU FPCR:
  //   - a denormal must not compare equal to zero (no flush to zero),
  //   - the scalar rounding mode (toward +infinity) must be used.
  TestFunction test([](HIRBuilder& b) {
    b.SetRoundingMode(b.LoadConstantInt32(2));  // toward +inf
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    auto* scalar_block = b.NewLabel();
    b.BranchTrue(LoadGPR(b, 4), scalar_block);
    StoreVR(b, 6, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 3), FLOAT32_TYPE));
    b.MarkLabel(scalar_block);
    StoreGPR(b, 3,
             b.ZeroExtend(b.CompareEQ(LoadFPR(b, 8), b.LoadZeroFloat64()),
                          INT64_TYPE));
    auto a = b.Convert(LoadFPR(b, 6), FLOAT32_TYPE);
    auto c = b.Convert(LoadFPR(b, 7), FLOAT32_TYPE);
    StoreFPR(b, 3, b.Convert(b.Add(a, c), FLOAT64_TYPE));
    b.Return();
  });
  for (uint64_t branch : {0, 1}) {
    test.Run(
        [branch](PPCContext* ctx) {
          ctx->r[4] = branch;
          ctx->v[4] = vec128f(1.0f, 2.0f, 3.0f, 4.0f);
          ctx->v[5] = vec128f(5.0f, 6.0f, 7.0f, 8.0f);
          ctx->f[6] = 1.0;
          ctx->f[7] = std::ldexp(1.0, -24);
          ctx->f[8] = std::numeric_limits<double>::denorm_min();
        },
        [&test](PPCContext* ctx) {
          REQUIRE(ctx->r[3] == 0);
          REQUIRE(static_cast<float>(ctx->f[3]) ==
                  std::nextafterf(1.0f, 2.0f));
          test.processors[0]->backend()->SetGuestRoundingMode(ctx, 0);
        });
  }
}

static void VmxFpcrNopBuiltin(PPCContext* ctx, void* arg0, void* arg1) {}

TEST_CASE("VMX_FPCR_PRESERVED_ACROSS_HOST_CALL", "[backend]") {
  // Host calls may change FPCR, which is restored for the mode that the
  // guest code was in when returning to it - VMX ops after the call must
  // still flush denormals, and scalar ops must still use the guest rounding
  // mode.
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  backend.reset(new xe::cpu::backend::x64::X64Backend());
#elif XE_ARCH_ARM64
  backend.reset(new xe::cpu::backend::a64::A64Backend());
#endif
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto* builtin_fn = processor->DefineBuiltin(
      "VmxFpcrNopBuiltin", VmxFpcrNopBuiltin, nullptr, nullptr);

  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn](HIRBuilder& b) {
        b.SetRoundingMode(b.LoadConstantInt32(2));  // toward +inf
        StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
        b.CallExtern(builtin_fn);
        StoreVR(b, 6, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
        b.CallExtern(builtin_fn);
        auto a = b.Convert(LoadFPR(b, 6), FLOAT32_TYPE);
        auto c = b.Convert(LoadFPR(b, 7), FLOAT32_TYPE);
        StoreFPR(b, 3, b.Convert(b.Add(a, c), FLOAT64_TYPE));
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;
  processor->backend()->SetGuestRoundingMode(ctx, 0);
  // The sum of two denormals is a denormal, flushed to zero in the VMX mode.
  float denormal = std::numeric_limits<float>::denorm_min() * 4;
  ctx->v[4] = vec128f(denormal);
  ctx->v[5] = vec128f(denormal);
  ctx->f[6] = 1.0;
  ctx->f[7] = std::ldexp(1.0, -24);

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  REQUIRE(ctx->v[3] == vec128f(0.0f));
  REQUIRE(ctx->v[6] == vec128f(0.0f));
  REQUIRE(static_cast<float>(ctx->f[3]) == std::nextafterf(1.0f, 2.0f));

  processor->backend()->SetGuestRoundingMode(ctx, 0);
  memory->SystemHeapFree(stack_address);
}
