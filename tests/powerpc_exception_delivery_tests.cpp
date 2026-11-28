#include "onix360/cpu/powerpc_interpreter.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

int main() {
    using namespace onix360::cpu;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
    };
    const auto d_form = [](unsigned op, unsigned rt, unsigned ra, unsigned imm) {
        return (op << 26U) | (rt << 21U) | (ra << 16U) | (imm & 0xFFFFU);
    };

    std::array<std::uint8_t, 8> ram{};
    XenonThreadState state{};
    state.reset(0x1000);
    check(execute_with_exception_delivery(state, d_form(32, 3, 0, 12), ram) ==
          StepResult::MemoryFault, "out-of-range LWZ reports memory fault");
    check(state.exception_pending && state.pc == 0x300U &&
          state.srr0 == 0x1000U && state.dar == 12U,
          "data-storage exception saves PC and fault address");

    state.reset(0x2000);
    check(execute_with_exception_delivery(state, d_form(40, 4, 0, 1), ram) ==
          StepResult::AlignmentFault, "unaligned LHZ reports alignment fault");
    check(state.exception_pending && state.pc == 0x600U &&
          state.srr0 == 0x2000U && state.dar == 1U,
          "alignment exception enters vector and records DAR");

    state.reset(0x3000);
    check(execute_with_exception_delivery(state, d_form(32, 3, 0, 0), ram) ==
          StepResult::Executed && !state.exception_pending && state.pc == 0x3004U,
          "successful memory access does not raise an exception");

    onix360::memory::PhysicalMemory physical(0x10000);
    onix360::memory::AddressSpace address_space;
    check(address_space.map_page(0x80000000,0x2000,3U)==onix360::memory::MemoryResult::Ok,"maps writable virtual page");
    check(address_space.map_page(0x80001000,0x3000,1U)==onix360::memory::MemoryResult::Ok,"maps read-only virtual page");
    onix360::cpu::AddressSpaceMemoryBus bus{&address_space,&physical,&state};
    const auto callbacks=bus.callbacks();
    state.reset(0x4000); state.gpr[3]=0x12345678;
    check(execute_with_exception_delivery(state,d_form(36,3,0,0x8000),{},nullptr,&callbacks)==StepResult::MemoryFault,"unmapped virtual store faults");
    state.reset(0x5000); state.gpr[4]=0x80001000;
    check(execute_with_exception_delivery(state,d_form(36,3,4,0),{},nullptr,&callbacks)==StepResult::MemoryFault && state.pc==0x300U,"read-only virtual page rejects store");
    state.reset(0x6000); state.gpr[4]=0x80001000;
    check(execute_with_exception_delivery(state,d_form(32,5,4,0),{},nullptr,&callbacks)==StepResult::Executed,"read-only virtual page allows load");
    // Instruction fetch is separately permission-checked (execute bit = 4).
    check(address_space.map_page(0x80002000,0x4000,5U)==onix360::memory::MemoryResult::Ok,
          "maps readable executable code page");
    const std::array<std::uint8_t,4> nop{0x60,0x00,0x00,0x00};
    check(physical.write(0x4000,nop)==onix360::memory::MemoryResult::Ok,
          "writes synthetic instruction bytes to physical RAM");
    std::uint32_t fetched=0;
    check(fetch_virtual_instruction(0x80002000,callbacks,fetched)==StepResult::Executed &&
          fetched==0x60000000U,"fetches big-endian instruction through executable mapping");
    check(fetch_virtual_instruction(0x80001000,callbacks,fetched)==StepResult::MemoryFault,
          "rejects instruction fetch from non-executable page");
    state.reset(0x80001000);
    check(fetch_and_execute_with_exception_delivery(state,callbacks)==StepResult::MemoryFault &&
          state.exception_pending && state.pc==0x400U && state.srr0==0x80001000U,
          "instruction storage exception records faulting PC");
    // RFI restores the saved instruction address and machine state,
    // then clears the pending exception marker.
    state.reset(0x700U);
    state.srr0 = 0x12345678U;
    state.srr1 = 0x0000000000009030ULL;
    state.msr = 0;
    state.exception_pending = true;
    state.exception_vector = 0x300U;
    check(execute_with_exception_delivery(state, 0x4C000064U) == StepResult::Executed,
          "RFI instruction executes from exception handler");
    check(state.pc == 0x12345678U && state.msr == 0x9030U,
          "RFI restores SRR0 into PC and SRR1 into MSR");
    check(!state.exception_pending && state.exception_vector == 0,
          "RFI clears pending exception state");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
