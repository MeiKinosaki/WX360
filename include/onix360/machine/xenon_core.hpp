#pragma once

#include "onix360/cpu/powerpc_interpreter.hpp"
#include "onix360/memory/physical_memory.hpp"
#include "onix360/memory/address_space.hpp"
#include "onix360/memory/xenon_address_layout.hpp"
#include "onix360/kernel/kernel_runtime.hpp"
#include "onix360/xex/xex_image_loader.hpp"
#include "onix360/audio/audio_engine.hpp"
#include "onix360/input/controller_state.hpp"
#include "onix360/input/xinput_backend.hpp"
#include "onix360/gpu/xenos_command_processor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace onix360::machine {

enum class RunResult : std::uint8_t {
    Executed,
    FetchFault,
    UnsupportedInstruction,
    DataMemoryFault,
    InstructionAlignmentFault,
    DataAlignmentFault,
    ExceptionTaken,
};

enum class LaunchResult : std::uint8_t { Started, EntryPointNotExecutable };

struct XenonCore {
    cpu::XenonThreadState state{};
    kernel::KernelRuntime* kernel_runtime{nullptr};
    gpu::XenosCommandProcessor gpu{};
    input::ControllerState controller{};
    input::XInputBackend xinput{};

    [[nodiscard]] static constexpr memory::XenonAddressRegionInfo
    classify_guest_address(std::uint32_t address) noexcept {
        return memory::classify_xenon_address(address);
    }

    [[nodiscard]] bool poll_controller(unsigned user_index = 0) noexcept {
        return xinput.poll(controller, user_index);
    }

    // Explicit subsystem bridges used by the machine scheduler and tests.
    void attach_kernel_runtime(kernel::KernelRuntime& runtime) noexcept {
        kernel_runtime = &runtime;
    }

    // Explicit cooperative guest-thread entry points. The caller decides
    // scheduling boundaries; run() remains a single-context bounded loop.
    [[nodiscard]] std::optional<std::uint32_t> create_guest_thread(
        const cpu::XenonThreadState& initial, std::uint8_t priority = 16) {
        return kernel_runtime ? kernel_runtime->create_guest_thread(initial, priority)
                              : std::nullopt;
    }
    [[nodiscard]] bool schedule_guest_thread() noexcept {
        return kernel_runtime && kernel_runtime->schedule_guest_thread(state);
    }
    [[nodiscard]] bool yield_guest_thread() noexcept {
        return kernel_runtime && kernel_runtime->yield_guest_thread(state);
    }
    [[nodiscard]] bool block_guest_thread() noexcept {
        return kernel_runtime && kernel_runtime->block_guest_thread(state);
    }
    [[nodiscard]] bool unblock_guest_thread(std::uint32_t id) noexcept {
        return kernel_runtime && kernel_runtime->unblock_guest_thread(id);
    }
    [[nodiscard]] bool terminate_guest_thread() noexcept {
        return kernel_runtime && kernel_runtime->terminate_guest_thread(state);
    }

    [[nodiscard]] bool execute_gpu_command_buffer(
        memory::PhysicalMemory& ram, std::uint64_t guest_address,
        std::size_t dword_count) {
        constexpr std::size_t max_command_dwords = 1U << 20U;
        if (dword_count == 0 || dword_count > max_command_dwords ||
            guest_address > std::numeric_limits<std::uint64_t>::max() - dword_count * 4U)
            return false;
        std::vector<std::uint32_t> commands;
        commands.reserve(dword_count);
        std::array<std::uint8_t, 4> bytes{};
        for (std::size_t i = 0; i < dword_count; ++i) {
            if (read_guest(ram, guest_address + i * 4U, bytes) != memory::MemoryResult::Ok)
                return false;
            commands.push_back((static_cast<std::uint32_t>(bytes[0]) << 24U) |
                (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                (static_cast<std::uint32_t>(bytes[2]) << 8U) |
                static_cast<std::uint32_t>(bytes[3]));
        }
        return gpu.execute_stream(commands);
    }

    [[nodiscard]] bool submit_audio_pcm16(
        const std::vector<std::int16_t>& interleaved_stereo) {
        return audio.submit_pcm16(interleaved_stereo);
    }
    [[nodiscard]] std::size_t pending_audio_frames() const noexcept {
        return audio.frames();
    }
    [[nodiscard]] bool play_audio_pending(std::uint32_t sample_rate = 48000U) noexcept {
        return audio.play_pending(sample_rate);
    }
    audio::AudioEngine audio{};
    memory::AddressSpace address_space{};
    bool mmu_enabled{false};
    // Experimental generic 32-bit PowerPC OEA HTAB path. This is not yet
    // the Xbox 360's custom hypervisor/L2 address-translation mechanism.
    memory::PowerPcHashedPageTable hashed_page_table{};
    bool hashed_mmu_enabled{false};

    [[nodiscard]] memory::MemoryResult configure_hashed_mmu(
        std::uint32_t sdr1, memory::PhysicalMemory& ram) noexcept {
        const auto result = hashed_page_table.configure_from_sdr1(sdr1, ram);
        hashed_mmu_enabled = result == memory::MemoryResult::Ok;
        return result;
    }

    [[nodiscard]] memory::MemoryResult map_guest_page(std::uint64_t virtual_address,
        std::uint64_t physical_address, std::uint8_t permissions) noexcept {
        const auto result = address_space.map_page(virtual_address, physical_address, permissions);
        if (result == memory::MemoryResult::Ok) mmu_enabled = true;
        return result;
    }

    struct InterpreterBusContext {
        XenonCore* core{};
        memory::PhysicalMemory* ram{};
    };

    [[nodiscard]] static std::uint64_t resolve_untranslated_guest_address(
        std::uint64_t guest_address) noexcept {
        if (guest_address <= 0xFFFFFFFFULL) {
            const auto alias = memory::xenon_physical_alias_address(
                static_cast<std::uint32_t>(guest_address));
            if (alias) return *alias;
        }
        return guest_address;
    }

    [[nodiscard]] static bool interpreter_read(
        void* opaque, std::uint64_t guest_address,
        std::span<std::uint8_t> output) noexcept {
        auto& context = *static_cast<InterpreterBusContext*>(opaque);
        auto& core = *context.core;
        auto& ram = *context.ram;
        const auto alias_address = resolve_untranslated_guest_address(guest_address);
        if (alias_address != guest_address)
            return ram.read(alias_address, output, memory::Access::Read) ==
                memory::MemoryResult::Ok;
        if (!core.mmu_enabled && !core.hashed_mmu_enabled)
            return ram.read(guest_address, output, memory::Access::Read) ==
                memory::MemoryResult::Ok;
        cpu::AddressSpaceMemoryBus bus{&core.address_space, &ram, &core.state,
            core.hashed_mmu_enabled ? &core.hashed_page_table : nullptr};
        const auto callbacks = bus.callbacks();
        return callbacks.read(callbacks.context, guest_address, output);
    }

    [[nodiscard]] static bool interpreter_write(
        void* opaque, std::uint64_t guest_address,
        std::span<const std::uint8_t> input) noexcept {
        auto& context = *static_cast<InterpreterBusContext*>(opaque);
        auto& core = *context.core;
        auto& ram = *context.ram;
        const auto alias_address = resolve_untranslated_guest_address(guest_address);
        if (alias_address != guest_address)
            return ram.write(alias_address, input) == memory::MemoryResult::Ok;
        if (!core.mmu_enabled && !core.hashed_mmu_enabled)
            return ram.write(guest_address, input) == memory::MemoryResult::Ok;
        cpu::AddressSpaceMemoryBus bus{&core.address_space, &ram, &core.state,
            core.hashed_mmu_enabled ? &core.hashed_page_table : nullptr};
        const auto callbacks = bus.callbacks();
        return callbacks.write(callbacks.context, guest_address, input);
    }

    [[nodiscard]] static bool interpreter_fetch(
        void* opaque, std::uint64_t guest_address,
        std::span<std::uint8_t> output) noexcept {
        auto& context = *static_cast<InterpreterBusContext*>(opaque);
        auto& core = *context.core;
        auto& ram = *context.ram;
        const auto alias_address = resolve_untranslated_guest_address(guest_address);
        if (alias_address != guest_address)
            return ram.read(alias_address, output, memory::Access::Execute) ==
                memory::MemoryResult::Ok;
        if (!core.mmu_enabled && !core.hashed_mmu_enabled)
            return ram.read(guest_address, output, memory::Access::Execute) ==
                memory::MemoryResult::Ok;
        cpu::AddressSpaceMemoryBus bus{&core.address_space, &ram, &core.state,
            core.hashed_mmu_enabled ? &core.hashed_page_table : nullptr};
        const auto callbacks = bus.callbacks();
        return callbacks.fetch(callbacks.context, guest_address, output);
    }

    // Install all PE section mappings emitted by the validated XEX loader.
    // This enables the software guest MMU before the first guest instruction.
    [[nodiscard]] memory::MemoryResult map_validated_xex(
        const xex::ValidatedXexImage& image) noexcept {
        const auto mappings = image.page_mappings();
        if (mappings.empty()) return memory::MemoryResult::Unmapped;
        const auto result = address_space.map_pages(mappings);
        if (result == memory::MemoryResult::Ok) mmu_enabled = true;
        return result;
    }

    // Only the loader-minted validation token can start execution.
    // Explicit guest data accessors share the same translator as instruction fetch.
    [[nodiscard]] memory::MemoryResult read_guest(
        memory::PhysicalMemory& ram, std::uint64_t guest_address,
        std::span<std::uint8_t> output) const noexcept {
        const auto alias_address = resolve_untranslated_guest_address(guest_address);
        if (alias_address != guest_address)
            return ram.read(alias_address, output, memory::Access::Read);
        if (hashed_mmu_enabled) {
            auto& self = *const_cast<XenonCore*>(this);
            cpu::AddressSpaceMemoryBus bus{&self.address_space, &ram, &self.state,
                                            &self.hashed_page_table};
            const auto translated = bus.translate(
                guest_address, output.size(), memory::Access::Read);
            if (translated.result != memory::MemoryResult::Ok) return translated.result;
            return ram.read(translated.physical_address, output, memory::Access::Read);
        }
        std::uint64_t physical_address = resolve_untranslated_guest_address(guest_address);
        if (mmu_enabled) {
            const auto translated = address_space.translate_range(
                guest_address, output.size(), memory::Access::Read, ram);
            if (translated.result != memory::MemoryResult::Ok) return translated.result;
            physical_address = translated.physical_address;
        }
        return ram.read(physical_address, output, memory::Access::Read);
    }

    [[nodiscard]] memory::MemoryResult write_guest(
        memory::PhysicalMemory& ram, std::uint64_t guest_address,
        std::span<const std::uint8_t> input) const noexcept {
        const auto alias_address = resolve_untranslated_guest_address(guest_address);
        if (alias_address != guest_address)
            return ram.write(alias_address, input);
        if (hashed_mmu_enabled) {
            auto& self = *const_cast<XenonCore*>(this);
            cpu::AddressSpaceMemoryBus bus{&self.address_space, &ram, &self.state,
                                            &self.hashed_page_table};
            const auto translated = bus.translate(
                guest_address, input.size(), memory::Access::Write);
            if (translated.result != memory::MemoryResult::Ok) return translated.result;
            return ram.write(translated.physical_address, input);
        }
        std::uint64_t physical_address = resolve_untranslated_guest_address(guest_address);
        if (mmu_enabled) {
            const auto translated = address_space.translate_range(
                guest_address, input.size(), memory::Access::Write, ram);
            if (translated.result != memory::MemoryResult::Ok) return translated.result;
            physical_address = translated.physical_address;
        }
        return ram.write(physical_address, input);
    }

    [[nodiscard]] LaunchResult start_xex(
        const xex::ValidatedXexImage& image, memory::PhysicalMemory& ram) noexcept {
        std::uint32_t first_instruction = 0;
        std::uint64_t fetch_address = image.entry_point();
        const std::uint64_t guest_entry =
            (mmu_enabled || hashed_mmu_enabled)
                ? image.guest_entry_point() : image.entry_point();
        if (hashed_mmu_enabled) {
            cpu::AddressSpaceMemoryBus bus{&address_space, &ram, &state,
                                            &hashed_page_table};
            const auto translated = bus.translate(
                guest_entry, 4, memory::Access::Execute);
            if (translated.result != memory::MemoryResult::Ok)
                return LaunchResult::EntryPointNotExecutable;
            fetch_address = translated.physical_address;
        } else if (mmu_enabled) {
            const auto translated = address_space.translate(
                guest_entry, memory::Access::Execute, ram);
            if (translated.result != memory::MemoryResult::Ok)
                return LaunchResult::EntryPointNotExecutable;
            fetch_address = translated.physical_address;
        }
        if (ram.fetch_u32(fetch_address, first_instruction) != memory::MemoryResult::Ok)
            return LaunchResult::EntryPointNotExecutable;
        const auto configured_segments = state.segment_registers;
        state.reset(guest_entry);
        if (hashed_mmu_enabled) state.segment_registers = configured_segments;
        return LaunchResult::Started;
    }

    [[nodiscard]] RunResult step(memory::PhysicalMemory& ram) noexcept {
        // Interim timing model: one DEC tick per interpreted instruction.
        // Replace with core-cycle accounting when instruction latency is modeled.
        state.advance_decrementer(1U);
        state.advance_timebase(1U);
        // Sample pending IRQs at instruction boundaries; masked requests stay latched.
        if (state.take_external_interrupt()) return RunResult::ExceptionTaken;
        if (state.take_decrementer_interrupt()) return RunResult::ExceptionTaken;
        if ((state.pc & 0x3U) != 0) {
            state.raise_exception(0x600U, state.pc);
            return RunResult::ExceptionTaken;
        }
        InterpreterBusContext bus_context{this, &ram};
        const cpu::MemoryAccess bus{&bus_context, &XenonCore::interpreter_read,
                                    &XenonCore::interpreter_write,
                                    &XenonCore::interpreter_fetch};
        std::uint32_t instruction = 0;
        const auto fetched = cpu::fetch_virtual_instruction(state.pc, bus, instruction);
        if (fetched != cpu::StepResult::Executed) {
            state.raise_exception(0x400U, state.pc);
            return RunResult::ExceptionTaken;
        }
        if (instruction == 0x44000042U) { // sc 2: XEX kernel import thunk
            if (kernel_runtime && kernel_runtime->dispatch_import_thunk(state.pc, state)) {
                state.pc += 4U;
                return RunResult::Executed;
            }
            state.raise_exception(0xC00U);
            return RunResult::ExceptionTaken;
        }
        if (instruction == 0x44000002U) { // sc 0: explicit development service
            if (kernel_runtime && kernel_runtime->dispatch(state)) {
                state.pc += 4U;
                return RunResult::Executed;
            }
            state.raise_exception(0xC00U);
            return RunResult::ExceptionTaken;
        }
        std::uint64_t fault_address = 0;
        const auto result = cpu::execute(state, instruction, ram.raw_bytes(),
                                         &fault_address, &bus);
        switch (result) {
        case cpu::StepResult::Executed: return RunResult::Executed;
        case cpu::StepResult::UnsupportedInstruction:
            state.raise_exception(0x700U);
            return RunResult::ExceptionTaken;
        case cpu::StepResult::MemoryFault:
            state.raise_exception(0x300U, fault_address);
            return RunResult::ExceptionTaken;
        case cpu::StepResult::AlignmentFault:
            state.raise_exception(0x600U, fault_address);
            return RunResult::ExceptionTaken;
        case cpu::StepResult::ProgramException:
            state.raise_exception(0x700U);
            return RunResult::ExceptionTaken;
        }
        return RunResult::UnsupportedInstruction;
    }
    struct RunBatchResult {
        std::size_t instructions_executed{0};
        RunResult stop_reason{RunResult::Executed};
        bool instruction_limit_reached{false};
    };

    // Bounded guest execution loop. It stops immediately on a fault or
    // unsupported instruction and never spins without an instruction budget.
    [[nodiscard]] RunBatchResult run(
        memory::PhysicalMemory& ram, std::size_t instruction_budget) noexcept {
        RunBatchResult batch{};
        if (instruction_budget == 0) {
            batch.instruction_limit_reached = true;
            return batch;
        }
        while (batch.instructions_executed < instruction_budget) {
            const RunResult result = step(ram);
            if (result != RunResult::Executed) {
                batch.stop_reason = result;
                return batch;
            }
            ++batch.instructions_executed;
        }
        batch.instruction_limit_reached = true;
        return batch;
    }

};

} // namespace onix360::machine
