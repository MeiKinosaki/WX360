#include "onix360/machine/xenon_core.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void kernel_test_service(onix360::cpu::XenonThreadState& state) noexcept {
    state.gpr[3] = 0xBEEFU;
}
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main() {
    using namespace onix360;
    memory::PhysicalMemory ram(0x4000);
    const std::array<std::uint8_t, 4> addi{0x38, 0x60, 0x00, 0x2A};
    check(ram.write(0x100, addi) == memory::MemoryResult::Ok,
          "place ADDI instruction in guest RAM");
    machine::XenonCore core;
    core.state.reset(0x100);
    check(core.step(ram) == machine::RunResult::Executed,
          "core fetches and executes instruction from guest RAM");
    check(core.state.gpr[3] == 42 && core.state.pc == 0x104,
          "core updates GPR and PC after instruction execution");
    memory::PhysicalMemory privilege_ram(0x4000);
    const std::uint32_t mfmsr_r3 = (31U << 26U) | (3U << 21U) | (83U << 1U);
    const std::array<std::uint8_t,4> mfmsr_bytes{
        static_cast<std::uint8_t>(mfmsr_r3 >> 24U),
        static_cast<std::uint8_t>(mfmsr_r3 >> 16U),
        static_cast<std::uint8_t>(mfmsr_r3 >> 8U),
        static_cast<std::uint8_t>(mfmsr_r3)};
    check(privilege_ram.write(0x100U, mfmsr_bytes) == memory::MemoryResult::Ok,
          "place privileged MFMSR instruction");
    machine::XenonCore privilege_core;
    privilege_core.state.reset(0x100U);
    privilege_core.state.msr = (std::uint64_t{1} << 14U);
    check(privilege_core.step(privilege_ram) == machine::RunResult::ExceptionTaken &&
          privilege_core.state.pc == 0x700U &&
          privilege_core.state.srr0 == 0x100U &&
          privilege_core.state.exception_vector == 0x700U,
          "privileged MFMSR delivers a program exception through vector 0x700");
    const std::array<std::uint8_t,4> alias_bytes{0x11,0x22,0x33,0x44};
    std::array<std::uint8_t,4> alias_readback{};
    check(core.write_guest(ram, 0xA0000200U, alias_bytes) == memory::MemoryResult::Ok &&
          core.read_guest(ram, 0xC0000200U, alias_readback) == memory::MemoryResult::Ok &&
          alias_readback == alias_bytes,
          "XenonCore resolves 64 KiB and 16 MiB physical aliases to shared RAM");

    // The real XenonCore step loop must use the segment/HTAB bus, not only
    // the standalone translation helper.
    memory::PhysicalMemory hashed_ram(1U << 20U);
    machine::XenonCore hashed_core;
    check(hashed_core.configure_hashed_mmu(0x00010000U, hashed_ram) ==
              memory::MemoryResult::Ok,
          "XenonCore configures HTAB from SDR1");
    constexpr std::uint32_t guest_vsid = 0x12345U;
    constexpr std::uint64_t guest_ea = 0x234U;
    check(hashed_core.hashed_page_table.insert(
              guest_vsid, guest_ea, 0x8000U, 0, hashed_ram) ==
              memory::MemoryResult::Ok,
          "XenonCore test installs guest segment PTE");
    const std::array<std::uint8_t,4> hashed_addi{0x38,0x60,0x00,0x2A};
    check(hashed_ram.write(0x8234U, hashed_addi) == memory::MemoryResult::Ok,
          "places instruction at translated physical address");
    hashed_core.state.reset(guest_ea);
    hashed_core.state.segment_registers[0] = guest_vsid;
    check(hashed_core.step(hashed_ram) == machine::RunResult::Executed &&
          hashed_core.state.gpr[3] == 42 && hashed_core.state.pc == guest_ea + 4U,
          "XenonCore fetch and execute uses segment VSID and HTAB PTE");
    const auto hashed_pte = hashed_core.hashed_page_table.lookup(
        guest_vsid, guest_ea, hashed_ram);
    std::array<std::uint8_t,4> hashed_pte1{};
    check(hashed_pte.result == memory::MemoryResult::Ok &&
          hashed_ram.read(hashed_pte.pte_address + 4U, hashed_pte1) ==
              memory::MemoryResult::Ok && (hashed_pte1[2] & 0x08U) != 0U,
          "XenonCore instruction fetch sets PTE reference bit");

    std::cerr << "CHECKPOINT 1\n";
    // The scheduler-facing run loop must honor its instruction budget.
    memory::PhysicalMemory batch_ram(0x4000);
    const std::array<std::uint8_t, 4> addi_r3_1{0x38, 0x60, 0x00, 0x01};
    const std::array<std::uint8_t, 4> addi_r4_2{0x38, 0x80, 0x00, 0x02};
    check(batch_ram.write(0x100, addi_r3_1) == memory::MemoryResult::Ok &&
          batch_ram.write(0x104, addi_r4_2) == memory::MemoryResult::Ok,
          "place two instructions for bounded execution");
    machine::XenonCore batch_core;
    batch_core.state.reset(0x100);
    const auto one_step = batch_core.run(batch_ram, 1);
    check(one_step.instructions_executed == 1 && one_step.instruction_limit_reached &&
          batch_core.state.gpr[3] == 1 && batch_core.state.gpr[4] == 0 &&
          batch_core.state.pc == 0x104,
          "run loop executes exactly the requested instruction budget");
    const auto zero_steps = batch_core.run(batch_ram, 0);
    check(zero_steps.instructions_executed == 0 && zero_steps.instruction_limit_reached &&
          batch_core.state.pc == 0x104,
          "zero instruction budget does not advance guest state");
    const auto next_step = batch_core.run(batch_ram, 1);
    check(next_step.instructions_executed == 1 && batch_core.state.gpr[4] == 2 &&
          batch_core.state.pc == 0x108,
          "run loop resumes at the next guest instruction");

    std::cerr << "CHECKPOINT 2\n";
    // A bounded batch must stop at the first guest exception and report
    // the reason without counting the faulting instruction as executed.
    memory::PhysicalMemory fault_batch_ram(0x4000);
    const std::array<std::uint8_t, 12> fault_batch_code{
        0x38, 0x60, 0x00, 0x01, // addi r3,r0,1
        0x38, 0x80, 0x00, 0x02, // addi r4,r0,2
        0x10, 0x00, 0x07, 0xFF  // unsupported vector instruction encoding
    };
    check(fault_batch_ram.write(0x100, fault_batch_code) == memory::MemoryResult::Ok,
          "place valid instructions followed by unsupported instruction");
    machine::XenonCore fault_batch_core;
    fault_batch_core.state.reset(0x100);
    const auto fault_batch = fault_batch_core.run(fault_batch_ram, 5);
    check(fault_batch.instructions_executed == 2 &&
          !fault_batch.instruction_limit_reached &&
          fault_batch.stop_reason == machine::RunResult::ExceptionTaken,
          "run loop stops and reports the first guest exception");
    check(fault_batch_core.state.gpr[3] == 1 && fault_batch_core.state.gpr[4] == 2 &&
          fault_batch_core.state.pc == 0x700 &&
          fault_batch_core.state.srr0 == 0x108,
          "faulting instruction is not counted and exception state is preserved");

    check(ram.set_page_permissions(0x100, memory::PhysicalMemory::page_size,
          static_cast<std::uint8_t>(memory::Access::Read)) == memory::MemoryResult::Ok,
          "remove execute permission from code page");
    core.state.pc = 0x100;
    check(core.step(ram) == machine::RunResult::ExceptionTaken,
          "core raises instruction-storage exception for non-executable page");
    check(core.state.pc == 0x400 && core.state.srr0 == 0x100 &&
          core.state.exception_pending && core.state.exception_vector == 0x400,
          "instruction-storage exception saves PC and selects vector 0x400");

    std::cerr << "CHECKPOINT 3\n";
    // The machine core owns the staged GPU, controller and audio queue.
    machine::XenonCore subsystem_core;
    check(subsystem_core.gpu.configure_framebuffer(4, 4),
          "machine-owned Xenos framebuffer configures");
    subsystem_core.gpu.framebuffer();
    subsystem_core.controller.buttons = input::A;
    check(subsystem_core.controller.pressed(input::A),
          "machine-owned controller state is accessible");
    check(subsystem_core.audio.submit_pcm16({100, -100}),
          "machine-owned PCM queue accepts a stereo frame");
    check(subsystem_core.audio.frames() == 1,
          "machine-owned PCM queue tracks submitted frame count");

    std::cerr << "CHECKPOINT 4\n";
    // Guest thread scheduling is exposed through the machine core.
    kernel::KernelRuntime thread_runtime;
    machine::XenonCore thread_core;
    thread_core.attach_kernel_runtime(thread_runtime);
    cpu::XenonThreadState low_priority{}, high_priority{};
    low_priority.reset(0x1000);
    high_priority.reset(0x2000);
    const auto high_id = thread_core.create_guest_thread(high_priority, 20);
    const auto low_id = thread_core.create_guest_thread(low_priority, 20);
    check(low_id && high_id, "machine core creates guest threads through kernel runtime");
    check(thread_core.schedule_guest_thread() && thread_core.state.pc == 0x2000,
          "machine core dispatches first equal-priority guest thread");
    thread_core.state.pc = 0x2010;
    check(thread_core.yield_guest_thread() && thread_core.state.pc == 0x1000,
          "machine core yields and switches guest execution context");
    check(thread_runtime.thread_scheduler().current_id() == low_id,
          "machine core exposes current scheduled guest thread");
    check(thread_core.terminate_guest_thread() && thread_core.state.pc == 0x2010,
          "machine core terminates thread and resumes ready context");
    check(thread_runtime.thread_scheduler().current_id() == high_id,
          "terminated guest thread is no longer current");

    std::cerr << "CHECKPOINT 5\n";
    // XEX launch authorization is covered by xex_synthetic_boot_tests,
    // which obtains the non-forgeable token from XexImageLoader. This unit
    // test intentionally exercises CPU/memory behavior only.

    std::cerr << "CHECKPOINT 6\n";
    core.state.pc = 0x5000;
    check(core.step(ram) == machine::RunResult::ExceptionTaken,
          "core raises instruction-storage exception outside physical RAM");
    check(core.state.pc == 0x400 && core.state.srr0 == 0x5000,
          "out-of-range instruction fetch records faulting PC");

    core.state.pc = 0x102;
    check(core.step(ram) == machine::RunResult::ExceptionTaken,
          "core raises alignment exception for misaligned instruction fetch");
    check(core.state.pc == 0x600 && core.state.srr0 == 0x102 &&
          core.state.dar == 0x102 && core.state.exception_vector == 0x600,
          "alignment exception saves PC, fault address and vector");
    memory::PhysicalMemory data_ram(0x4000);
    const std::array<std::uint8_t, 4> lwz_r4_r3{0x80, 0x83, 0x00, 0x00};
    check(data_ram.write(0x200, lwz_r4_r3) == memory::MemoryResult::Ok,
          "place LWZ instruction for data-alignment exception test");
    machine::XenonCore data_core;
    data_core.state.reset(0x200);
    data_core.state.gpr[3] = 0x101;
    check(data_core.step(data_ram) == machine::RunResult::ExceptionTaken,
          "misaligned LWZ enters alignment exception");
    check(data_core.state.pc == 0x600 && data_core.state.srr0 == 0x200 &&
          data_core.state.dar == 0x101,
          "data alignment exception saves effective address in DAR");

    std::cerr << "CHECKPOINT 7\n";
    // Exercise guest virtual instruction fetch through the page translator.
    memory::PhysicalMemory virtual_ram(0x4000);
    check(virtual_ram.write(0x100, addi) == memory::MemoryResult::Ok,
          "place instruction at translated physical address");
    machine::XenonCore virtual_core;
    virtual_core.state.reset(0x1100);
    check(virtual_core.map_guest_page(0x1000, 0x0000,
          static_cast<std::uint8_t>(memory::Access::Read) |
          static_cast<std::uint8_t>(memory::Access::Write) |
          static_cast<std::uint8_t>(memory::Access::Execute)) == memory::MemoryResult::Ok,
          "map guest virtual page to physical page");
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed,
          "fetches instruction through virtual address translation");
    check(virtual_core.state.gpr[3] == 42 && virtual_core.state.pc == 0x1104,
          "translated fetch preserves virtual PC progression");

    std::cerr << "CHECKPOINT 8\n";
    // D-form data instructions must translate their effective virtual address,
    // not index the physical RAM span directly.
    const std::array<std::uint8_t, 4> lwz_virtual{0x80, 0x83, 0x00, 0x00};
    const std::array<std::uint8_t, 4> stw_virtual{0x90, 0x83, 0x00, 0x04};
    const std::array<std::uint8_t, 4> physical_value{0xDE, 0xAD, 0xBE, 0xEF};
    check(virtual_ram.write(0x104, lwz_virtual) == memory::MemoryResult::Ok &&
          virtual_ram.write(0x108, stw_virtual) == memory::MemoryResult::Ok,
          "place virtual LWZ and STW instructions in translated code page");
    check(virtual_ram.write(0x2000, physical_value) == memory::MemoryResult::Ok,
          "initialize data at translated physical page");
    check(virtual_core.map_guest_page(0x3000, 0x2000,
          static_cast<std::uint8_t>(memory::Access::Read) |
          static_cast<std::uint8_t>(memory::Access::Write)) == memory::MemoryResult::Ok,
          "map guest data page to a different physical page");
    virtual_core.state.pc = 0x1104;
    virtual_core.state.gpr[3] = 0x3000;
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed &&
          virtual_core.state.gpr[4] == 0xDEADBEEFU,
          "LWZ reads through the guest virtual-to-physical mapping");
    virtual_core.state.pc = 0x1108;
    virtual_core.state.gpr[4] = 0x12345678U;
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed,
          "STW writes through the guest virtual-to-physical mapping");
    std::array<std::uint8_t, 4> stored_value{};
    check(virtual_ram.read(0x2004, stored_value) == memory::MemoryResult::Ok &&
          stored_value == std::array<std::uint8_t, 4>{0x12, 0x34, 0x56, 0x78},
          "STW updates translated physical memory in big-endian order");

    check(virtual_core.map_guest_page(0x4000, 0x2000,
          static_cast<std::uint8_t>(memory::Access::Read)) == memory::MemoryResult::Ok,
          "map a read-only guest data page");
    const std::array<std::uint8_t, 4> stw_read_only{0x90, 0x83, 0x00, 0x00};
    check(virtual_ram.write(0x10C, stw_read_only) == memory::MemoryResult::Ok,
          "place STW instruction for guest write-protection test");
    virtual_core.state.pc = 0x110C;
    virtual_core.state.gpr[3] = 0x4000;
    check(virtual_core.step(virtual_ram) == machine::RunResult::ExceptionTaken &&
          virtual_core.state.exception_vector == 0x300,
          "STW to read-only virtual page raises data-storage exception");
    check(virtual_core.state.dar == 0x4000,
          "data-storage protection exception records faulting guest address in DAR");
    const std::array<std::uint8_t, 4> lwz_unmapped{0x80, 0x83, 0x00, 0x00};
    check(virtual_ram.write(0x118, lwz_unmapped) == memory::MemoryResult::Ok,
          "place LWZ for unmapped guest data test");
    virtual_core.state.pc = 0x1118;
    virtual_core.state.gpr[3] = 0x6000;
    check(virtual_core.step(virtual_ram) == machine::RunResult::ExceptionTaken &&
          virtual_core.state.exception_vector == 0x300 &&
          virtual_core.state.dar == 0x6000,
          "unmapped guest data access raises DSI and records DAR");
    virtual_core.state.pc = 0x9000;
    check(virtual_core.step(virtual_ram) == machine::RunResult::ExceptionTaken &&
          virtual_core.state.exception_vector == 0x400 &&
          virtual_core.state.dar == 0x9000,
          "unmapped guest instruction fetch raises ISI and records faulting PC");

    std::cerr << "CHECKPOINT 9\n";
    // Indexed LWZX/STWX use the same translated guest memory bus.
    const auto xform = [](unsigned rt, unsigned ra, unsigned rb, unsigned xo) {
        return (31U << 26U) | (rt << 21U) | (ra << 16U) | (rb << 11U) | (xo << 1U);
    };
    const auto encode_be = [](std::uint32_t word) {
        return std::array<std::uint8_t, 4>{
            static_cast<std::uint8_t>(word >> 24U),
            static_cast<std::uint8_t>(word >> 16U),
            static_cast<std::uint8_t>(word >> 8U),
            static_cast<std::uint8_t>(word)};
    };
    const auto lwzx = encode_be(xform(4, 3, 5, 23));
    const auto stwx = encode_be(xform(4, 3, 5, 151));
    check(virtual_ram.write(0x110, lwzx) == memory::MemoryResult::Ok &&
          virtual_ram.write(0x114, stwx) == memory::MemoryResult::Ok,
          "place indexed LWZX and STWX instructions");
    check(virtual_core.map_guest_page(0x5000, 0x2000,
          static_cast<std::uint8_t>(memory::Access::Read) |
          static_cast<std::uint8_t>(memory::Access::Write)) == memory::MemoryResult::Ok,
          "map indexed data virtual page");
    virtual_core.state.gpr[3] = 0x5000;
    virtual_core.state.gpr[5] = 0;
    virtual_core.state.pc = 0x1110;
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed &&
          virtual_core.state.gpr[4] == 0xDEADBEEFU,
          "LWZX loads from translated indexed virtual address");
    virtual_core.state.gpr[4] = 0xCAFEBABEU;
    virtual_core.state.pc = 0x1114;
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed,
          "STWX stores to translated indexed virtual address");
    check(virtual_ram.read(0x2000, stored_value) == memory::MemoryResult::Ok &&
          stored_value == std::array<std::uint8_t, 4>{0xCA, 0xFE, 0xBA, 0xBE},
          "STWX updates translated physical memory");
    const auto lbzx = encode_be(xform(6, 3, 5, 87));
    const auto stbx = encode_be(xform(7, 3, 5, 215));
    const auto lhzx = encode_be(xform(8, 3, 5, 279));
    const auto sthx = encode_be(xform(9, 3, 5, 407));
    check(virtual_ram.write(0x118, lbzx) == memory::MemoryResult::Ok &&
          virtual_ram.write(0x11C, stbx) == memory::MemoryResult::Ok &&
          virtual_ram.write(0x120, lhzx) == memory::MemoryResult::Ok &&
          virtual_ram.write(0x124, sthx) == memory::MemoryResult::Ok,
          "place indexed byte and halfword instructions");
    virtual_core.state.gpr[3] = 0x5000;
    virtual_core.state.gpr[5] = 1;
    virtual_core.state.pc = 0x1118;
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed &&
          virtual_core.state.gpr[6] == 0xFEU,
          "LBZX reads byte through translated guest memory");
    virtual_core.state.gpr[5] = 2;
    virtual_core.state.gpr[7] = 0x77U;
    virtual_core.state.pc = 0x111C;
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed,
          "STBX writes byte through translated guest memory");
    std::array<std::uint8_t, 4> indexed_bytes{};
    check(virtual_ram.read(0x2000, indexed_bytes) == memory::MemoryResult::Ok &&
          indexed_bytes == std::array<std::uint8_t, 4>{0xCA, 0xFE, 0x77, 0xBE},
          "STBX changes only indexed translated byte");
    virtual_core.state.gpr[5] = 0;
    virtual_core.state.pc = 0x1120;
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed &&
          virtual_core.state.gpr[8] == 0xCAFEU,
          "LHZX reads big-endian halfword through guest MMU");
    virtual_core.state.gpr[9] = 0x1234U;
    virtual_core.state.pc = 0x1124;
    check(virtual_core.step(virtual_ram) == machine::RunResult::Executed,
          "STHX writes halfword through guest MMU");
    check(virtual_ram.read(0x2000, indexed_bytes) == memory::MemoryResult::Ok &&
          indexed_bytes == std::array<std::uint8_t, 4>{0x12, 0x34, 0x77, 0xBE},
          "STHX updates translated memory in big-endian order");

    std::cerr << "CHECKPOINT 10\n";
    const std::array<std::uint8_t, 4> guest_data{0x11, 0x22, 0x33, 0x44};
    check(virtual_core.write_guest(virtual_ram, 0x1120, guest_data) == memory::MemoryResult::Ok,
          "writes data through guest virtual address");
    std::array<std::uint8_t, 4> guest_read{};
    check(virtual_core.read_guest(virtual_ram, 0x1120, guest_read) == memory::MemoryResult::Ok &&
          guest_read == guest_data, "reads data through guest virtual address");
    check(virtual_core.write_guest(virtual_ram, 0x1120, guest_data) ==
          memory::MemoryResult::Ok, "guest write translation remains available");
    check(virtual_core.read_guest(virtual_ram, 0x2120, guest_read) ==
          memory::MemoryResult::Unmapped, "guest data access faults on unmapped page");


    std::cerr << "CHECKPOINT 11\n";
    // Kernel SC dispatch is connected to the machine's CPU execution path.
    kernel::KernelRuntime runtime;
    check(runtime.register_service(0x42U, &kernel_test_service),
          "register host-backed kernel service");
    machine::XenonCore integrated_core;
    integrated_core.attach_kernel_runtime(runtime);
    integrated_core.state.reset(0x300);
    integrated_core.state.gpr[0] = 0x42U;
    const std::array<std::uint8_t, 8> sc_then_addi{
        0x44, 0x00, 0x00, 0x02, // sc: dispatch service 0x42
        0x38, 0x83, 0x00, 0x01  // addi r4,r3,1 consumes service result
    };
    check(data_ram.write(0x300, sc_then_addi) == memory::MemoryResult::Ok,
          "write system call and dependent instruction for integrated kernel path");
    const auto kernel_batch = integrated_core.run(data_ram, 2);
    check(kernel_batch.instructions_executed == 2 &&
          kernel_batch.instruction_limit_reached &&
          kernel_batch.stop_reason == machine::RunResult::Executed,
          "bounded CPU run loop executes system call and resumes guest code");
    check(integrated_core.state.gpr[3] == 0xBEEFU &&
          integrated_core.state.gpr[4] == 0xBEF0U &&
          integrated_core.state.pc == 0x308,
          "guest instruction consumes register result returned by kernel service");

    std::cerr << "CHECKPOINT 12\n";
    // Guest big-endian PM4 command buffer flows through guest memory into GPU.
    check(integrated_core.gpu.configure_framebuffer(4, 4),
          "configure integrated GPU framebuffer");
    const std::uint32_t clear_header = (3U << 30U) |
        (static_cast<std::uint32_t>(gpu::XenosCommandProcessor::onix360_clear_opcode) << 8U);
    const std::uint32_t present_header = (3U << 30U) |
        (static_cast<std::uint32_t>(gpu::XenosCommandProcessor::onix360_present_opcode) << 8U);
    const std::array<std::uint32_t, 4> gpu_words{
        clear_header, 0xFF123456U, present_header, 0U};
    std::array<std::uint8_t, 16> gpu_bytes{};
    for (std::size_t i = 0; i < gpu_words.size(); ++i) {
        gpu_bytes[i * 4U] = static_cast<std::uint8_t>(gpu_words[i] >> 24U);
        gpu_bytes[i * 4U + 1U] = static_cast<std::uint8_t>(gpu_words[i] >> 16U);
        gpu_bytes[i * 4U + 2U] = static_cast<std::uint8_t>(gpu_words[i] >> 8U);
        gpu_bytes[i * 4U + 3U] = static_cast<std::uint8_t>(gpu_words[i]);
    }
    check(data_ram.write(0x800, gpu_bytes) == memory::MemoryResult::Ok,
          "write guest PM4 command buffer");
    check(integrated_core.execute_gpu_command_buffer(data_ram, 0x800, gpu_words.size()),
          "machine reads and submits guest PM4 command buffer");
    check(integrated_core.gpu.framebuffer().front_buffer().size() == 16U &&
          integrated_core.gpu.framebuffer().front_buffer()[0] == 0xFF123456U,
          "GPU command buffer clears and presents framebuffer");

    std::cerr << "CHECKPOINT 13\n";
    // CPU machine owns the audio producer queue; device output remains explicit.
    check(integrated_core.submit_audio_pcm16({321, -321, 123, -123}) &&
          integrated_core.pending_audio_frames() == 2U,
          "machine routes stereo PCM samples into attached audio engine");

    std::cerr << "CHECKPOINT 14\n";
    // The machine instruction-fetch path must honor Execute independently
    // from Read: executable-only guest pages are valid code mappings.
    memory::PhysicalMemory execute_only_ram(0x4000);
    const std::array<std::uint8_t, 4> execute_only_addi{0x38, 0x60, 0x00, 0x07};
    check(execute_only_ram.write(0x1000, execute_only_addi) == memory::MemoryResult::Ok,
          "store synthetic instruction in physical backing page");
    machine::XenonCore execute_only_core;
    check(execute_only_core.map_guest_page(0x9000, 0x1000,
          static_cast<std::uint8_t>(memory::Access::Execute)) == memory::MemoryResult::Ok,
          "map guest code page with Execute but without Read permission");
    execute_only_core.state.reset(0x9000);
    check(execute_only_core.step(execute_only_ram) == machine::RunResult::Executed &&
          execute_only_core.state.gpr[3] == 7U &&
          execute_only_core.state.pc == 0x9004U,
          "XenonCore fetches and executes from Execute-only virtual page");

    if (failures == 0) std::cout << "Onix360 Xenon core integration tests passed\n";
    // External IRQ requests remain latched while MSR[EE] is disabled,
    // then enter vector 0x500 at the next instruction boundary.
    memory::PhysicalMemory irq_ram(0x4000);
    const std::array<std::uint8_t, 4> irq_nop{0x60, 0x00, 0x00, 0x00};
    check(irq_ram.write(0x300, irq_nop) == memory::MemoryResult::Ok,
          "write synthetic instruction before external interrupt test");
    machine::XenonCore irq_core;
    irq_core.state.reset(0x300);
    irq_core.state.msr = 0;
    irq_core.state.request_external_interrupt();
    check(irq_core.step(irq_ram) == machine::RunResult::Executed &&
          irq_core.state.pc == 0x304 &&
          irq_core.state.external_interrupt_pending,
          "masked external interrupt remains pending while guest executes");
    irq_core.state.msr = 0x8000U;
    const auto saved_irq_pc = irq_core.state.pc;
    check(irq_core.step(irq_ram) == machine::RunResult::ExceptionTaken &&
          irq_core.state.pc == 0x500U &&
          irq_core.state.srr0 == saved_irq_pc &&
          irq_core.state.srr1 == 0x8000U &&
          !irq_core.state.external_interrupt_pending,
          "enabled external interrupt enters vector and saves guest context");

    // Decrementer requests use the PPC decrementer exception vector and
    // remain pending while external exceptions are globally masked.
    memory::PhysicalMemory dec_ram(0x4000);
    check(dec_ram.write(0x600, irq_nop) == memory::MemoryResult::Ok,
          "write synthetic instruction for decrementer interrupt test");
    machine::XenonCore dec_core;
    dec_core.state.reset(0x600);
    dec_core.state.request_decrementer_interrupt();
    check(dec_core.step(dec_ram) == machine::RunResult::Executed &&
          dec_core.state.pc == 0x604 &&
          dec_core.state.decrementer_interrupt_pending,
          "masked decrementer interrupt remains pending");
    dec_core.state.msr = 0x8000U;
    const auto saved_dec_pc = dec_core.state.pc;
    check(dec_core.step(dec_ram) == machine::RunResult::ExceptionTaken &&
          dec_core.state.pc == 0x900U &&
          dec_core.state.srr0 == saved_dec_pc &&
          dec_core.state.srr1 == 0x8000U &&
          !dec_core.state.decrementer_interrupt_pending,
          "enabled decrementer interrupt enters vector 0x900 and saves context");
    // Time Base is a split 64-bit counter with independently readable halves.
    cpu::XenonThreadState tb_state{};
    tb_state.reset(0x700);
    tb_state.advance_timebase(0xFFFFFFFEULL);
    check(tb_state.read_timebase_upper() == 0U &&
          tb_state.read_timebase_lower() == 0xFFFFFFFEU,
          "timebase exposes lower counter before wrap");
    tb_state.advance_timebase(3U);
    check(tb_state.read_timebase_upper() == 1U &&
          tb_state.read_timebase_lower() == 1U,
          "timebase carries into upper half on 32-bit wrap");
    tb_state.write_timebase_lower(0xAABBCCDDU);
    check(tb_state.read_timebase_upper() == 1U &&
          tb_state.read_timebase_lower() == 0xAABBCCDDU,
          "writing TBL preserves TBU");
    tb_state.write_timebase_upper(0x11223344U);
    check(tb_state.read_timebase_upper() == 0x11223344U &&
          tb_state.read_timebase_lower() == 0xAABBCCDDU,
          "writing TBU preserves TBL");
    tb_state.reset();
    check(tb_state.timebase_ticks == 0U,
          "CPU reset clears emulated timebase");

    // DEC underflow latches the decrementer exception request.
    cpu::XenonThreadState timer_state{};
    timer_state.reset(0x700);
    timer_state.write_decrementer(3U);
    timer_state.advance_decrementer(2U);
    check(timer_state.decrementer == 1U &&
          !timer_state.decrementer_interrupt_pending,
          "decrementer counts down without premature interrupt");
    timer_state.advance_decrementer(2U);
    check(timer_state.decrementer == 0xFFFFFFFFU &&
          timer_state.decrementer_interrupt_pending,
          "decrementer underflow latches interrupt request");
    timer_state.write_decrementer(10U);
    check(timer_state.decrementer_interrupt_pending,
          "writing DEC does not silently discard a latched request");
    dec_core.state.request_decrementer_interrupt();
    dec_core.state.reset(0x600);
    check(!dec_core.state.decrementer_interrupt_pending,
          "CPU reset clears pending decrementer interrupt");

    // XenonCore advances the provisional DEC clock once per interpreted
    // instruction and delivers underflow at the following instruction boundary.
    check(dec_ram.write(0x604, irq_nop) == memory::MemoryResult::Ok,
          "write second synthetic instruction for integrated DEC timing");
    machine::XenonCore timed_core;
    timed_core.state.reset(0x600);
    timed_core.state.msr = 0x8000U;
    timed_core.state.write_decrementer(1U);
    check(timed_core.step(dec_ram) == machine::RunResult::Executed &&
          timed_core.state.decrementer == 0U &&
          timed_core.state.timebase_ticks == 1U &&
          timed_core.state.pc == 0x604U,
          "DEC advances by one provisional tick after first instruction");
    check(timed_core.step(dec_ram) == machine::RunResult::ExceptionTaken &&
          timed_core.state.pc == 0x900U &&
          timed_core.state.srr0 == 0x604U &&
          timed_core.state.timebase_ticks == 2U &&
          timed_core.state.decrementer_interrupt_pending == false,
          "DEC underflow is delivered before the next instruction executes");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
