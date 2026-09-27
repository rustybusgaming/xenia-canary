/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_assembler.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <mutex>

#include "third_party/capstone/include/capstone/aarch64.h"
#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/reset_scope.h"
#include "xenia/base/string.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DEFINE_path(a64_jit_map_path, "",
            "Folder to write a map of the generated ARM64 code to, for "
            "matching samples of the process (such as from the macOS sample "
            "tool) to guest functions. For every function, jit_map.txt "
            "contains the host and guest address ranges, the offsets of the "
            "code in jit_host.bin and jit_guest.bin, and the source map. "
            "Empty to disable.",
            "a64");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using xe::cpu::hir::HIRBuilder;

namespace {

class JitMapWriter {
 public:
  static JitMapWriter& Get() {
    static JitMapWriter writer;
    return writer;
  }

  void Write(GuestFunction* function, const void* machine_code,
             size_t code_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Open()) {
      return;
    }
    // Guest end addresses are the address of the last instruction.
    uint32_t guest_address = function->address();
    uint32_t guest_size = 4;
    if (function->end_address() >= guest_address) {
      guest_size = std::min(function->end_address() - guest_address + 4,
                            uint32_t(1024 * 1024));
    }
    const void* guest_code =
        function->module()->memory()->TranslateVirtual(guest_address);
    std::string line =
        fmt::format("{:X} {:X} {:08X} {:X} {:X} {:X}",
                    reinterpret_cast<uintptr_t>(machine_code), code_size,
                    guest_address, guest_size, host_offset_, guest_offset_);
    for (const SourceMapEntry& entry : function->source_map()) {
      line +=
          fmt::format(" {:08X}:{:X}", entry.guest_address, entry.code_offset);
    }
    line += '\n';
    std::fwrite(machine_code, 1, code_size, host_file_);
    std::fwrite(guest_code, 1, guest_size, guest_file_);
    std::fwrite(line.data(), 1, line.size(), map_file_);
    host_offset_ += code_size;
    guest_offset_ += guest_size;
    std::fflush(host_file_);
    std::fflush(guest_file_);
    std::fflush(map_file_);
  }

 private:
  bool Open() {
    if (opened_) {
      return map_file_ != nullptr;
    }
    opened_ = true;
    const std::filesystem::path& folder = cvars::a64_jit_map_path;
    std::filesystem::create_directories(folder);
    map_file_ = xe::filesystem::OpenFile(folder / "jit_map.txt", "wb");
    host_file_ = xe::filesystem::OpenFile(folder / "jit_host.bin", "wb");
    guest_file_ = xe::filesystem::OpenFile(folder / "jit_guest.bin", "wb");
    if (!map_file_ || !host_file_ || !guest_file_) {
      XELOGE("A64: Failed to create the JIT map files in {}",
             xe::path_to_utf8(folder));
      for (FILE* file : {map_file_, host_file_, guest_file_}) {
        if (file) {
          std::fclose(file);
        }
      }
      map_file_ = host_file_ = guest_file_ = nullptr;
      return false;
    }
    XELOGI("A64: Writing the JIT map to {}", xe::path_to_utf8(folder));
    return true;
  }

  std::mutex mutex_;
  bool opened_ = false;
  FILE* map_file_ = nullptr;
  FILE* host_file_ = nullptr;
  FILE* guest_file_ = nullptr;
  uint64_t host_offset_ = 0;
  uint64_t guest_offset_ = 0;
};

}  // namespace

A64Assembler::A64Assembler(A64Backend* backend)
    : Assembler(backend), a64_backend_(backend), capstone_handle_(0) {
  if (cs_open(CS_ARCH_AARCH64, CS_MODE_ARM, &capstone_handle_) != CS_ERR_OK) {
    assert_always("Failed to initialize capstone for ARM64");
  }
  cs_option(capstone_handle_, CS_OPT_DETAIL, CS_OPT_OFF);
}

A64Assembler::~A64Assembler() {
  emitter_.reset();

  if (capstone_handle_) {
    cs_close(&capstone_handle_);
  }
}

bool A64Assembler::Initialize() {
  if (!Assembler::Initialize()) {
    return false;
  }

  emitter_.reset(new A64Emitter(a64_backend_, &allocator_));

  return true;
}

void A64Assembler::Reset() {
  string_buffer_.Reset();
  Assembler::Reset();
}

bool A64Assembler::Assemble(GuestFunction* function, HIRBuilder* builder,
                            uint32_t debug_info_flags,
                            std::unique_ptr<FunctionDebugInfo> debug_info) {
  SCOPE_profile_cpu_f("cpu");

  // Reset when we leave.
  xe::make_reset_scope(this);

  // Lower HIR -> ARM64.
  void* machine_code = nullptr;
  size_t code_size = 0;
  if (!emitter_->Emit(function, builder, debug_info_flags, debug_info.get(),
                      &machine_code, &code_size, &function->source_map())) {
    return false;
  }

  // Stash generated machine code.
  if (debug_info_flags & DebugInfoFlags::kDebugInfoDisasmMachineCode) {
    DumpMachineCode(machine_code, code_size, function->source_map(),
                    &string_buffer_);
    debug_info->set_machine_code_disasm(xe_strdup(string_buffer_.buffer()));
    string_buffer_.Reset();
  }

  function->set_debug_info(std::move(debug_info));
  static_cast<A64Function*>(function)->Setup(
      reinterpret_cast<uint8_t*>(machine_code), code_size);

  if (!cvars::a64_jit_map_path.empty()) {
    JitMapWriter::Get().Write(function, machine_code, code_size);
  }

  // Install into indirection table.
  uint64_t host_address = reinterpret_cast<uint64_t>(machine_code);
  assert_true((host_address >> 32) == 0);
  reinterpret_cast<A64CodeCache*>(backend_->code_cache())
      ->AddIndirection(function->address(),
                       static_cast<uint32_t>(host_address));

  return true;
}

void A64Assembler::DumpMachineCode(
    void* machine_code, size_t code_size,
    const std::vector<SourceMapEntry>& source_map, StringBuffer* str) {
  if (source_map.empty()) {
    return;
  }
  auto source_map_index = 0;
  uint32_t next_code_offset = source_map[0].code_offset;

  const uint8_t* code_ptr = reinterpret_cast<uint8_t*>(machine_code);
  size_t remaining_code_size = code_size;
  uint64_t address = uint64_t(machine_code);
  cs_insn insn = {0};
  while (remaining_code_size &&
         cs_disasm_iter(capstone_handle_, &code_ptr, &remaining_code_size,
                        &address, &insn)) {
    // Look up source offset.
    auto code_offset =
        uint32_t(code_ptr - reinterpret_cast<uint8_t*>(machine_code));
    if (code_offset >= next_code_offset &&
        source_map_index < source_map.size()) {
      auto& source_map_entry = source_map[source_map_index];
      str->AppendFormat("{:08X} ", source_map_entry.guest_address);
      ++source_map_index;
      next_code_offset = source_map_index < source_map.size()
                             ? source_map[source_map_index].code_offset
                             : UINT_MAX;
    } else {
      str->Append("         ");
    }

    str->AppendFormat("{:08X}      {:<6} {}\n", uint32_t(insn.address),
                      insn.mnemonic, insn.op_str);
  }
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
