#pragma once

#include "onix360/cpu/xenon_state.hpp"
#include "onix360/memory/address_space.hpp"
#include "onix360/memory/powerpc_hashed_page_table.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <limits>
#include <algorithm>

namespace onix360::cpu {

enum class StepResult {
    Executed,
    UnsupportedInstruction,
    MemoryFault,
    AlignmentFault,
    ProgramException,
};

struct MemoryAccess {
    void* context{};
    bool (*read)(void*, std::uint64_t, std::span<std::uint8_t>) noexcept{};
    bool (*write)(void*, std::uint64_t, std::span<const std::uint8_t>) noexcept{};
    bool (*fetch)(void*, std::uint64_t, std::span<std::uint8_t>) noexcept{};
};

// PowerPC segment-register fields: T=direct-store, Ks/Kp keys, N=execute inhibit.
// disables instruction fetches from the selected 256 MiB segment.
inline constexpr std::uint32_t segment_no_execute_mask = std::uint32_t{1} << 28U;
inline constexpr std::uint32_t segment_key_supervisor_mask = std::uint32_t{1} << 30U;
inline constexpr std::uint32_t segment_key_problem_mask = std::uint32_t{1} << 29U;
inline constexpr std::uint32_t segment_direct_store_mask = std::uint32_t{1} << 31U;
inline constexpr std::uint32_t segment_translation_mask = segment_direct_store_mask; // legacy name
inline constexpr std::uint32_t segment_vsid_mask = 0x00FFFFFFU;

struct SegmentRegisterFields {
    std::uint32_t vsid{};
    bool key_supervisor{};
    bool key_problem{};
    bool no_execute{};
    bool direct_store{};
};

[[nodiscard]] constexpr SegmentRegisterFields decode_segment_register(
    std::uint32_t value) noexcept {
    return {value & segment_vsid_mask,
            (value & segment_key_supervisor_mask) != 0,
            (value & segment_key_problem_mask) != 0,
            (value & segment_no_execute_mask) != 0,
            (value & segment_translation_mask) != 0};
}

// Hashed-page-table hash inputs for 4 KiB PowerPC pages. Full PTE walking
// is implemented by PowerPcHashedPageTable and the memory-bus integration below.
[[nodiscard]] constexpr std::uint32_t segment_page_index(
    std::uint64_t effective_address) noexcept {
    return static_cast<std::uint32_t>((effective_address >> 12U) & 0xFFFFU);
}
[[nodiscard]] constexpr std::uint32_t segment_primary_hash(
    std::uint32_t vsid, std::uint64_t effective_address) noexcept {
    return (vsid & segment_vsid_mask) ^ segment_page_index(effective_address);
}
[[nodiscard]] constexpr std::uint32_t segment_secondary_hash(
    std::uint32_t vsid, std::uint64_t effective_address) noexcept {
    return ~segment_primary_hash(vsid, effective_address);
}

[[nodiscard]] inline bool segment_allows_instruction_fetch(
    const XenonThreadState& state, std::uint64_t effective_address) noexcept {
    const unsigned segment = static_cast<unsigned>((effective_address >> 28U) & 0xFU);
    return !decode_segment_register(state.segment_registers[segment]).no_execute;
}

struct AddressSpaceMemoryBus {
    memory::AddressSpace* address_space{};
    memory::PhysicalMemory* physical_memory{};
    XenonThreadState* thread_state{};
    memory::PowerPcHashedPageTable* hashed_page_table{};

    [[nodiscard]] memory::MemoryResult configure_htab_from_sdr1(
        std::uint32_t sdr1) noexcept {
        if (!hashed_page_table || !physical_memory)
            return memory::MemoryResult::Unmapped;
        return hashed_page_table->configure_from_sdr1(sdr1, *physical_memory);
    }

    [[nodiscard]] static constexpr bool pte_allows_access(
        std::uint8_t pp, bool key, memory::Access access) noexcept {
        if (pp > 3U) return false;
        if (!key) {
            if (access == memory::Access::Write) return pp != 3U;
            return true;
        }
        if (pp == 0U) return false;
        if (access == memory::Access::Write) return pp == 2U;
        return true;
    }

    [[nodiscard]] memory::AddressSpace::Translation translate(
        std::uint64_t effective_address, std::size_t length,
        memory::Access access) const noexcept {
        if (!address_space || !physical_memory)
            return {memory::MemoryResult::Unmapped, 0};
        const bool use_hashed_table = hashed_page_table && hashed_page_table->configured();
        if (use_hashed_table && !thread_state)
            return {memory::MemoryResult::Unmapped, 0};
        if (thread_state) {
            const unsigned segment =
                static_cast<unsigned>((effective_address >> 28U) & 0xFU);
            const auto fields = decode_segment_register(
                thread_state->segment_registers[segment]);
            if (fields.direct_store) return {memory::MemoryResult::Unmapped, 0};
        }
        if (use_hashed_table) {
            if (length == 0 ||
                length - 1U > std::numeric_limits<std::uint64_t>::max() -
                                  effective_address)
                return {memory::MemoryResult::OutOfRange, 0};
            const bool problem_state = (thread_state->msr & (1ULL << 14U)) != 0;
            std::uint64_t first_physical = 0;
            std::size_t completed = 0;
            while (completed < length) {
                const auto current_ea = effective_address + completed;
                const unsigned current_segment =
                    static_cast<unsigned>((current_ea >> 28U) & 0xFU);
                const auto current_fields = decode_segment_register(
                    thread_state->segment_registers[current_segment]);
                if (current_fields.direct_store)
                    return {memory::MemoryResult::Unmapped, 0};
                if (access == memory::Access::Execute && current_fields.no_execute)
                    return {memory::MemoryResult::PermissionDenied, 0};
                const bool key = problem_state
                    ? current_fields.key_problem : current_fields.key_supervisor;
                const auto pte = hashed_page_table->lookup(
                    current_fields.vsid, current_ea, *physical_memory);
                if (pte.result != memory::MemoryResult::Ok)
                    return {pte.result, 0};
                if (!pte_allows_access(pte.pp, key, access))
                    return {memory::MemoryResult::PermissionDenied, 0};
                const auto page_remaining = memory::PhysicalMemory::page_size -
                    static_cast<std::size_t>(current_ea & 0xFFFU);
                const auto chunk = (std::min)(length - completed, page_remaining);
                if (completed == 0) first_physical = pte.physical_address;
                else if (pte.physical_address != first_physical + completed)
                    return {memory::MemoryResult::Unmapped, 0};
                const auto permission = physical_memory->check_access(
                    pte.physical_address, chunk, access);
                if (permission != memory::MemoryResult::Ok)
                    return {permission, 0};
                const auto reference_status = hashed_page_table->mark_access(
                    pte, access, *physical_memory);
                if (reference_status != memory::MemoryResult::Ok)
                    return {reference_status, 0};
                completed += chunk;
            }
            return {memory::MemoryResult::Ok, first_physical};
        }
        return address_space->translate_range(
            effective_address, length, access, *physical_memory);
    }

    [[nodiscard]] MemoryAccess callbacks() noexcept {
        return {this,
            +[](void* c, std::uint64_t a, std::span<std::uint8_t> out) noexcept {
                auto& b = *static_cast<AddressSpaceMemoryBus*>(c);
                const auto p = b.translate(a, out.size(), memory::Access::Read);
                return p.result == memory::MemoryResult::Ok &&
                    b.physical_memory->read(p.physical_address, out) ==
                        memory::MemoryResult::Ok;
            },
            +[](void* c, std::uint64_t a, std::span<const std::uint8_t> in) noexcept {
                auto& b = *static_cast<AddressSpaceMemoryBus*>(c);
                const auto p = b.translate(a, in.size(), memory::Access::Write);
                return p.result == memory::MemoryResult::Ok &&
                    b.physical_memory->write(p.physical_address, in) ==
                        memory::MemoryResult::Ok;
            },
            +[](void* c, std::uint64_t a, std::span<std::uint8_t> out) noexcept {
                auto& b = *static_cast<AddressSpaceMemoryBus*>(c);
                if (!b.thread_state ||
                    !segment_allows_instruction_fetch(*b.thread_state, a))
                    return false;
                const auto p = b.translate(a, out.size(), memory::Access::Execute);
                return p.result == memory::MemoryResult::Ok &&
                    b.physical_memory->read(p.physical_address, out,
                        memory::Access::Execute) == memory::MemoryResult::Ok;
            }};
    }
};

struct DecodedInstruction {
    std::uint32_t raw{};
    std::uint8_t primary_opcode{};
    std::uint8_t rt{};
    std::uint8_t ra{};
    std::uint8_t rb{};
    std::uint16_t xo{};
    std::int16_t immediate{};
    bool record{};
};

[[nodiscard]] constexpr std::uint64_t rotate_left_32(std::uint32_t value, unsigned shift) noexcept {
    shift &= 31U;
    if (shift == 0) return value;
    return static_cast<std::uint32_t>((value << shift) | (value >> (32U - shift)));
}

[[nodiscard]] constexpr std::uint32_t mask32(unsigned begin, unsigned end) noexcept {
    std::uint32_t mask = 0;
    for (unsigned bit = begin & 31U;; bit = (bit + 1U) & 31U) {
        mask |= (std::uint32_t{1} << (31U - bit));
        if (bit == (end & 31U)) break;
    }
    return mask;
}

inline void update_cr0(XenonThreadState& state, std::uint64_t value,
                       std::uint64_t sign_bit) noexcept {
    std::uint8_t field = 0;
    if ((value & sign_bit) != 0) field |= 0x8U;
    else if (value != 0) field |= 0x4U;
    else field |= 0x2U;
    if ((state.xer & (std::uint64_t{1} << 31U)) != 0) field |= 0x1U;
    state.set_condition_field(0, field);
}

[[nodiscard]] constexpr DecodedInstruction decode(std::uint32_t word) noexcept {
    return {
        word,
        static_cast<std::uint8_t>(word >> 26U),
        static_cast<std::uint8_t>((word >> 21U) & 0x1FU),
        static_cast<std::uint8_t>((word >> 16U) & 0x1FU),
        static_cast<std::uint8_t>((word >> 11U) & 0x1FU),
        static_cast<std::uint16_t>((word >> 1U) & 0x3FFU),
        static_cast<std::int16_t>(word & 0xFFFFU),
        (word & 1U) != 0
    };
}

[[nodiscard]] inline StepResult execute(
    XenonThreadState& state, std::uint32_t instruction,
    std::span<std::uint8_t> memory = {},
    std::uint64_t* fault_address = nullptr,
    const MemoryAccess* bus = nullptr) noexcept {
    const auto d = decode(instruction);
    const auto read_memory = [&](std::uint64_t address, std::span<std::uint8_t> output) noexcept {
        if (bus && bus->read) return bus->read(bus->context, address, output);
        if (address > memory.size() || output.size() > memory.size() - static_cast<std::size_t>(address))
            return false;
        for (std::size_t i = 0; i < output.size(); ++i)
            output[i] = memory[static_cast<std::size_t>(address) + i];
        return true;
    };
    const auto write_memory = [&](std::uint64_t address, std::span<const std::uint8_t> input) noexcept {
        bool written = false;
        if (bus && bus->write) {
            written = bus->write(bus->context, address, input);
        } else {
            if (address > memory.size() || input.size() > memory.size() - static_cast<std::size_t>(address))
                return false;
            for (std::size_t i = 0; i < input.size(); ++i)
                memory[static_cast<std::size_t>(address) + i] = input[i];
            written = true;
        }
        if (written && state.reservation_valid && !input.empty()) {
            const std::uint64_t span = static_cast<std::uint64_t>(input.size() - 1U);
            if (span > std::numeric_limits<std::uint64_t>::max() - address) {
                state.reservation_valid = false; // Conservatively invalidate wrapped ranges.
            } else {
                const std::uint64_t first = address & ~std::uint64_t{31};
                const std::uint64_t last = (address + span) & ~std::uint64_t{31};
                const std::uint64_t reserved_granule = state.reservation_address & ~std::uint64_t{31};
                if (first <= reserved_granule && reserved_granule <= last)
                    state.reservation_valid = false;
            }
        }
        return written;
    };
    std::uint64_t result = 0;
    bool writes_result = false;
    bool record_32bit = false;
    std::uint8_t destination = d.rt;
    bool branch_taken = false;
    std::uint64_t branch_target = 0;
    bool update_base = false;
    std::uint64_t updated_address = 0;

    switch (d.primary_opcode) {
    case 18: { // b
        std::int32_t displacement = static_cast<std::int32_t>(instruction & 0x03FFFFFCU);
        if ((displacement & 0x02000000) != 0) displacement |= static_cast<std::int32_t>(0xFC000000U);
        const bool absolute = (instruction & 2U) != 0;
        const bool link = (instruction & 1U) != 0;
        branch_target = absolute ? static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement))
                                 : static_cast<std::uint64_t>(static_cast<std::int64_t>(state.pc) + displacement);
        if (link) state.lr = state.pc + 4U;
        branch_taken = true;
        break;
    }
    case 16: { // bc
        const unsigned bo = (instruction >> 21U) & 0x1FU;
        const unsigned bi = (instruction >> 16U) & 0x1FU;
        bool condition_ok = (bo & 0x10U) != 0;
        if (!condition_ok) {
            const unsigned cr_bit = (state.cr >> (31U - bi)) & 1U;
            condition_ok = cr_bit == ((bo >> 3U) & 1U);
        }
        bool count_ok = true;
        if ((bo & 0x04U) == 0) {
            --state.ctr;
            count_ok = ((state.ctr != 0) != ((bo & 0x02U) != 0));
        }
        const std::int32_t displacement = static_cast<std::int16_t>(instruction & 0xFFFCU);
        const bool absolute = (instruction & 2U) != 0;
        const bool link = (instruction & 1U) != 0;
        branch_taken = condition_ok && count_ok;
        if (branch_taken) {
            branch_target = absolute ? static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement))
                                     : static_cast<std::uint64_t>(static_cast<std::int64_t>(state.pc) + displacement);
        }
        if (link) state.lr = state.pc + 4U;
        break;
    }
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45: { // D-form byte/halfword/word loads and stores
        const std::int64_t displacement = d.immediate;
        const std::uint64_t base = d.ra == 0 ? 0 : state.gpr[d.ra];
        const std::uint64_t address = base + static_cast<std::uint64_t>(displacement);
        const bool word_access = d.primary_opcode == 32 || d.primary_opcode == 33 ||
                                 d.primary_opcode == 36 || d.primary_opcode == 37;
        const bool halfword_access = d.primary_opcode >= 40 && d.primary_opcode <= 45;
        const bool load = d.primary_opcode <= 35 || (d.primary_opcode >= 40 && d.primary_opcode <= 43);
        const bool update = d.primary_opcode == 33 || d.primary_opcode == 35 ||
                            d.primary_opcode == 37 || d.primary_opcode == 39 ||
                            d.primary_opcode == 41 || d.primary_opcode == 43 || d.primary_opcode == 45;
        if (update && d.ra == 0) return StepResult::UnsupportedInstruction;
        if (update && load && d.ra == d.rt) return StepResult::UnsupportedInstruction;
        const std::size_t width = word_access ? 4U : (halfword_access ? 2U : 1U);
        if (width > 1U && (address % width) != 0U) {
            if (fault_address) *fault_address = address;
            return StepResult::AlignmentFault;
        }
        std::array<std::uint8_t, 4> bytes{};
        if (load) {
            if (!read_memory(address, std::span<std::uint8_t>(bytes.data(), width))) {
                if (fault_address) *fault_address = address;
                return StepResult::MemoryFault;
            }
            if (word_access) {
                result = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                         (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
            } else if (halfword_access) {
                const std::uint16_t value = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
                result = (d.primary_opcode == 42 || d.primary_opcode == 43)
                    ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(value)))
                    : value;
            } else {
                result = bytes[0];
            }
            writes_result = true;
        } else {
            if (word_access) {
                const std::uint32_t value = static_cast<std::uint32_t>(state.gpr[d.rt]);
                bytes[0] = static_cast<std::uint8_t>(value >> 24U);
                bytes[1] = static_cast<std::uint8_t>(value >> 16U);
                bytes[2] = static_cast<std::uint8_t>(value >> 8U);
                bytes[3] = static_cast<std::uint8_t>(value);
            } else if (halfword_access) {
                const std::uint16_t value = static_cast<std::uint16_t>(state.gpr[d.rt]);
                bytes[0] = static_cast<std::uint8_t>(value >> 8U);
                bytes[1] = static_cast<std::uint8_t>(value);
            } else {
                bytes[0] = static_cast<std::uint8_t>(state.gpr[d.rt]);
            }
            if (!write_memory(address, std::span<const std::uint8_t>(bytes.data(), width))) {
                if (fault_address) *fault_address = address;
                return StepResult::MemoryFault;
            }
        }
        if (update) { update_base = true; updated_address = address; }
        break;
    }
    case 46: case 47: { // lmw/stmw: transfer GPR[RT..31] as big-endian words.
        const std::uint64_t base = d.ra == 0 ? 0U : state.gpr[d.ra];
        const std::uint64_t address = base +
            static_cast<std::uint64_t>(static_cast<std::int64_t>(d.immediate));
        const std::size_t count = static_cast<std::size_t>(32U - d.rt) * 4U;
        if ((address & 3U) != 0U) {
            if (fault_address) *fault_address = address;
            return StepResult::AlignmentFault;
        }
        std::array<std::uint8_t, 128> bytes{};
        if (d.primary_opcode == 46U) {
            // Stage the complete transfer before changing any guest register.
            if (!read_memory(address, std::span<std::uint8_t>(bytes.data(), count))) {
                if (fault_address) *fault_address = address;
                return StepResult::MemoryFault;
            }
            for (unsigned reg = d.rt; reg < 32U; ++reg) {
                const std::size_t offset = static_cast<std::size_t>(reg - d.rt) * 4U;
                state.gpr[reg] = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
                    (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
                    (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
                    bytes[offset + 3U];
            }
        } else {
            for (unsigned reg = d.rt; reg < 32U; ++reg) {
                const std::size_t offset = static_cast<std::size_t>(reg - d.rt) * 4U;
                const std::uint32_t value = static_cast<std::uint32_t>(state.gpr[reg]);
                bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
                bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 16U);
                bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 8U);
                bytes[offset + 3U] = static_cast<std::uint8_t>(value);
            }
            if (!write_memory(address, std::span<const std::uint8_t>(bytes.data(), count))) {
                if (fault_address) *fault_address = address;
                return StepResult::MemoryFault;
            }
        }
        break;
    }
    case 8: { // subfic: RT = SIMM - RA; XER[CA] indicates no borrow
        const std::uint64_t lhs = static_cast<std::uint64_t>(static_cast<std::int64_t>(d.immediate));
        const std::uint64_t rhs = state.gpr[d.ra];
        result = lhs - rhs;
        constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
        state.xer = lhs >= rhs ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
        writes_result = true;
        break;
    }
    case 12: case 13: { // addic / addic.
        const std::uint64_t lhs = state.gpr[d.ra];
        const std::uint64_t rhs = static_cast<std::uint64_t>(static_cast<std::int64_t>(d.immediate));
        result = lhs + rhs;
        const bool carry = result < lhs;
        constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
        state.xer = carry ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
        writes_result = true;
        // ADDIC. updates CR0; ADDIC does not.
        if (d.primary_opcode == 13) update_cr0(state, result, std::uint64_t{1} << 63U);
        break;
    }
    case 24: case 25: case 26: case 27: case 28: case 29: { // immediate logical operations
        const std::uint64_t imm = static_cast<std::uint16_t>(instruction & 0xFFFFU);
        const std::uint64_t shifted_imm = imm << ((d.primary_opcode == 25 ||
                                                   d.primary_opcode == 27 ||
                                                   d.primary_opcode == 29) ? 16U : 0U);
        const std::uint64_t source = state.gpr[d.rt];
        switch (d.primary_opcode) {
        case 24: case 25: // ori / oris
            result = source | shifted_imm;
            break;
        case 26: case 27: // xori / xoris
            result = source ^ shifted_imm;
            break;
        case 28: case 29: // andi. / andis.
            result = source & shifted_imm;
            break;
        default:
            return StepResult::UnsupportedInstruction;
        }
        destination = d.ra;
        writes_result = true;
        // ANDI. and ANDIS. always update CR0; they have no Rc bit.
        if (d.primary_opcode == 28 || d.primary_opcode == 29)
            update_cr0(state, result, std::uint64_t{1} << 63U);
        break;
    }
    case 7: { // mulli: signed low-word multiply by sign-extended immediate
        const std::uint64_t rhs = static_cast<std::uint64_t>(static_cast<std::int64_t>(d.immediate));
        result = state.gpr[d.ra] * rhs; // Unsigned arithmetic gives the low 64 bits without signed-overflow UB.
        writes_result = true;
        break;
    }
    case 14: // addi: RA=0 denotes literal zero, not GPR[0].
        result = (d.ra == 0 ? 0U : state.gpr[d.ra]) +
                 static_cast<std::uint64_t>(static_cast<std::int64_t>(d.immediate));
        writes_result = true;
        break;
    case 15: // addis: RA=0 denotes literal zero, not GPR[0].
        result = (d.ra == 0 ? 0U : state.gpr[d.ra]) +
                 (static_cast<std::uint64_t>(static_cast<std::int64_t>(d.immediate)) << 16U);
        writes_result = true;
        break;
    case 20: { // rlwimi: rotate word then insert selected mask bits into RA.
        const unsigned shift = (instruction >> 11U) & 0x1FU;
        const unsigned mb = (instruction >> 6U) & 0x1FU;
        const unsigned me = (instruction >> 1U) & 0x1FU;
        const std::uint32_t mask = mask32(mb, me);
        const auto rotated = static_cast<std::uint32_t>(rotate_left_32(
            static_cast<std::uint32_t>(state.gpr[d.rt]), shift));
        const auto previous = static_cast<std::uint32_t>(state.gpr[d.ra]);
        result = (previous & ~mask) | (rotated & mask);
        destination = d.ra;
        writes_result = true;
        record_32bit = true;
        break;
    }
    case 21: { // rlwinm
        const unsigned shift = (instruction >> 11U) & 0x1FU;
        const unsigned mb = (instruction >> 6U) & 0x1FU;
        const unsigned me = (instruction >> 1U) & 0x1FU;
        const auto rotated = static_cast<std::uint32_t>(rotate_left_32(
            static_cast<std::uint32_t>(state.gpr[d.rt]), shift));
        result = rotated & mask32(mb, me);
        destination = d.ra;
        writes_result = true;
        record_32bit = true;
        break;
    }
    case 23: { // rlwnm: rotate word by RB[0:4], then apply a wrapping mask.
        const unsigned shift = static_cast<unsigned>(state.gpr[d.rb] & 0x1FU);
        const unsigned mb = (instruction >> 6U) & 0x1FU;
        const unsigned me = (instruction >> 1U) & 0x1FU;
        const auto rotated = static_cast<std::uint32_t>(rotate_left_32(
            static_cast<std::uint32_t>(state.gpr[d.rt]), shift));
        result = rotated & mask32(mb, me);
        destination = d.ra;
        writes_result = true;
        record_32bit = true;
        break;
    }
    case 19: {
        if (d.xo == 0U) { // mcrf: copy one CR field to another.
            const unsigned bf = (instruction >> 23U) & 7U;
            const unsigned bfa = (instruction >> 18U) & 7U;
            state.set_condition_field(bf, state.condition_field(bfa));
            break;
        }
        if (d.xo == 257U || d.xo == 129U || d.xo == 289U ||
            d.xo == 225U || d.xo == 33U || d.xo == 449U ||
            d.xo == 417U || d.xo == 193U) { // CR logical bit operations.
            const unsigned bt = (instruction >> 21U) & 0x1FU;
            const unsigned ba = (instruction >> 16U) & 0x1FU;
            const unsigned bb = (instruction >> 11U) & 0x1FU;
            const auto cr_bit = [&state](unsigned bit) noexcept {
                return (state.cr >> (31U - bit)) & 1U;
            };
            const unsigned a = cr_bit(ba), b = cr_bit(bb);
            unsigned value = 0;
            switch (d.xo) {
            case 257: value = a & b; break;          // crand
            case 129: value = a & (b ^ 1U); break;   // crandc
            case 289: value = a ^ b ^ 1U; break;     // creqv
            case 225: value = (a & b) ^ 1U; break;   // crnand
            case 33:  value = (a | b) ^ 1U; break;   // crnor
            case 449: value = a | b; break;          // cror
            case 417: value = a | (b ^ 1U); break;   // crorc
            case 193: value = a ^ b; break;          // crxor
            default: break;
            }
            const std::uint32_t mask = std::uint32_t{1} << (31U - bt);
            state.cr = (state.cr & ~mask) | (static_cast<std::uint32_t>(value) << (31U - bt));
            break;
        }
        if (d.xo == 528U) { // bcctr: conditional branch to CTR (CTR is not decremented).
            const unsigned bo = (instruction >> 21U) & 0x1FU;
            const unsigned bi = (instruction >> 16U) & 0x1FU;
            // The architecture requires BO[2] = 1 for BCCTR; CTR is the target.
            if ((bo & 0x04U) == 0) return StepResult::UnsupportedInstruction;
            bool condition_ok = (bo & 0x10U) != 0;
            if (!condition_ok) {
                const unsigned cr_bit = (state.cr >> (31U - bi)) & 1U;
                condition_ok = cr_bit == ((bo >> 3U) & 1U);
            }
            if (condition_ok) {
                branch_target = state.ctr & ~std::uint64_t{3};
                branch_taken = true;
            }
            if ((instruction & 1U) != 0) state.lr = state.pc + 4U;
            break;
        }
        if (d.xo == 16U) { // bclr: conditional branch to LR.
            const unsigned bo = (instruction >> 21U) & 0x1FU;
            const unsigned bi = (instruction >> 16U) & 0x1FU;
            bool condition_ok = (bo & 0x10U) != 0;
            if (!condition_ok) {
                const unsigned cr_bit = (state.cr >> (31U - bi)) & 1U;
                condition_ok = cr_bit == ((bo >> 3U) & 1U);
            }
            bool count_ok = true;
            if ((bo & 0x04U) == 0) {
                --state.ctr;
                count_ok = ((state.ctr != 0) != ((bo & 0x02U) != 0));
            }
            if (condition_ok && count_ok) {
                branch_target = state.lr & ~std::uint64_t{3};
                branch_taken = true;
            }
            if ((instruction & 1U) != 0) state.lr = state.pc + 4U;
            break;
        }
        // XL-form privileged return from interrupt (RFI), XO=50.
        if (d.xo != 50U) return StepResult::UnsupportedInstruction;
        branch_target = state.srr0;
        state.msr = state.srr1;
        state.clear_exception();
        branch_taken = true;
        break;
    }
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: { // lfs/lfd/stfs/stfd D-form.
        const bool single = d.primary_opcode == 48 || d.primary_opcode == 49 ||
                            d.primary_opcode == 52 || d.primary_opcode == 53;
        const bool load = d.primary_opcode <= 51;
        const bool update = (d.primary_opcode & 1U) != 0;
        if (update && d.ra == 0) return StepResult::UnsupportedInstruction;
        const std::uint64_t address = (d.ra == 0 ? 0 : state.gpr[d.ra]) +
            static_cast<std::uint64_t>(static_cast<std::int64_t>(d.immediate));
        const std::size_t width = single ? 4U : 8U;
        if ((address & (width - 1U)) != 0U) {
            if (fault_address) *fault_address = address;
            return StepResult::AlignmentFault;
        }
        std::array<std::uint8_t, 8> bytes{};
        if (load && !read_memory(address, std::span<std::uint8_t>(bytes.data(), width))) {
            if (fault_address) *fault_address = address;
            return StepResult::MemoryFault;
        }
        std::uint64_t raw = 0;
        if (load) {
            for (std::size_t n = 0; n < width; ++n) raw = (raw << 8U) | bytes[n];
        }
        if (load) {
            if (single) {
                const float f = std::bit_cast<float>(static_cast<std::uint32_t>(raw));
                state.fpr[d.rt] = std::bit_cast<std::uint64_t>(static_cast<double>(f));
            } else {
                state.fpr[d.rt] = raw;
            }
        } else {
            if (single) raw = std::bit_cast<std::uint32_t>(
                static_cast<float>(std::bit_cast<double>(state.fpr[d.rt])));
            else raw = state.fpr[d.rt];
            for (std::size_t n = 0; n < width; ++n)
                bytes[n] = static_cast<std::uint8_t>(raw >> ((width - 1U - n) * 8U));
            if (!write_memory(address, std::span<const std::uint8_t>(bytes.data(), width))) {
                if (fault_address) *fault_address = address;
                return StepResult::MemoryFault;
            }
        }
        if (update) state.gpr[d.ra] = address;
        break;
    }
    case 59: { // Single-precision FPU arithmetic (A-form).
        const double a64 = std::bit_cast<double>(state.fpr[d.ra]);
        const double b64 = std::bit_cast<double>(state.fpr[d.rb]);
        const float a = static_cast<float>(a64);
        const float b = static_cast<float>(b64);
        const unsigned fp_xo = (instruction >> 1U) & 0x1FU;
        float value = 0.0F;
        switch (fp_xo) {
        case 18: value = a / b; break; // fdivs
        case 20: value = a - b; break; // fsubs
        case 21: value = a + b; break; // fadds
        case 25: { // fmuls: FRC is encoded separately from FRB.
            const unsigned frc = (instruction >> 6U) & 0x1FU;
            value = a * static_cast<float>(std::bit_cast<double>(state.fpr[frc]));
            break;
        }
        case 28: case 29: case 30: case 31: {
            const unsigned frc = (instruction >> 6U) & 0x1FU;
            const float c = static_cast<float>(std::bit_cast<double>(state.fpr[frc]));
            const float product_addend = fp_xo == 28U || fp_xo == 30U ? -b : b;
            value = std::fma(a, c, product_addend);
            if (fp_xo == 30U || fp_xo == 31U) value = -value;
            break;
        }
        default: return StepResult::UnsupportedInstruction;
        }
        const double rounded = static_cast<double>(value);
        if (std::isnan(value) && !std::isnan(a) && !std::isnan(b))
            state.fpscr |= 0xA0000000U; // FX | VX
        if (std::isinf(value) && std::isfinite(a) && std::isfinite(b))
            state.fpscr |= 0x90000000U; // FX | OX
        if (fp_xo == 18U && b == 0.0F && a != 0.0F)
            state.fpscr |= 0x84000000U; // FX | ZX
        state.fpr[d.rt] = std::bit_cast<std::uint64_t>(rounded);
        if ((instruction & 1U) != 0)
            state.set_condition_field(1, static_cast<std::uint8_t>((state.fpscr >> 28U) & 0xFU));
        break;
    }
    case 63: { // Floating-point arithmetic, compare, and conversion subset.
        const double a = std::bit_cast<double>(state.fpr[d.ra]);
        const double b = std::bit_cast<double>(state.fpr[d.rb]);
        const unsigned fp_xo = (instruction >> 1U) & 0x1FU;
        if (d.xo == 38U || d.xo == 70U) { // mtfsb1 / mtfsb0
            const unsigned bit = (instruction >> 21U) & 0x1FU;
            const std::uint32_t mask = std::uint32_t{1} << (31U - bit);
            if (d.xo == 38U) state.fpscr |= mask;
            else state.fpscr &= ~mask;
            if ((instruction & 1U) != 0)
                state.set_condition_field(1,
                    static_cast<std::uint8_t>((state.fpscr >> 28U) & 0xFU));
            break;
        }
        if (d.xo == 134U) { // mtfsfi: write an immediate into one FPSCR field.
            const unsigned bf = (instruction >> 23U) & 7U;
            const unsigned immediate = (instruction >> 12U) & 0xFU;
            const unsigned shift = 28U - bf * 4U;
            const std::uint32_t mask = 0xFU << shift;
            state.fpscr = (state.fpscr & ~mask) |
                (static_cast<std::uint32_t>(immediate) << shift);
            if ((instruction & 1U) != 0)
                state.set_condition_field(1,
                    static_cast<std::uint8_t>((state.fpscr >> 28U) & 0xFU));
            break;
        }
        if (d.xo == 64U) { // mcrfs: copy an FPSCR field into a CR field.
            const unsigned bf = (instruction >> 23U) & 7U;
            const unsigned bfa = (instruction >> 18U) & 7U;
            const unsigned shift = 28U - bfa * 4U;
            const auto field = static_cast<std::uint8_t>(
                (state.fpscr >> shift) & 0xFU);
            state.set_condition_field(bf, field);
            break;
        }
        if (d.xo == 583U) { // mffs
            state.fpr[d.rt] = static_cast<std::uint64_t>(state.fpscr);
            if ((instruction & 1U) != 0)
                state.set_condition_field(1, static_cast<std::uint8_t>((state.fpscr >> 28U) & 0xFU));
            break;
        }
        if (d.xo == 711U) { // mtfsf
            const unsigned field_mask = (instruction >> 17U) & 0xFFU;
            const auto source = static_cast<std::uint32_t>(state.fpr[d.rb]);
            for (unsigned field = 0; field < 8; ++field) {
                if ((field_mask & (1U << (7U - field))) == 0U) continue;
                const unsigned shift = 28U - field * 4U;
                const std::uint32_t mask = 0xFU << shift;
                state.fpscr = (state.fpscr & ~mask) | (source & mask);
            }
            if ((instruction & 1U) != 0)
                state.set_condition_field(1, static_cast<std::uint8_t>((state.fpscr >> 28U) & 0xFU));
            break;
        }
        if (d.xo == 0U || d.xo == 32U) { // fcmpu / fcmpo
            const unsigned bf = (instruction >> 23U) & 7U;
            std::uint8_t field = 0x1U;
            if (!std::isnan(a) && !std::isnan(b))
                field = a < b ? 0x8U : (a > b ? 0x4U : 0x2U);
            state.set_condition_field(bf, field);
            if (std::isnan(a) || std::isnan(b)) state.fpscr |= (1U << 3U);
            break;
        }
        // Unary X-form operations use the full 10-bit XO, not the
        // five-bit A-form arithmetic XO field.
        if (d.xo == 40U || d.xo == 72U ||
            d.xo == 136U || d.xo == 264U) {
            double unary = a;
            if (d.xo == 40U) unary = -a;                 // fneg
            else if (d.xo == 72U) unary = a;             // fmr
            else if (d.xo == 136U) unary = -std::fabs(a); // fnabs
            else unary = std::fabs(a);                  // fabs
            state.fpr[d.rt] = std::bit_cast<std::uint64_t>(unary);
            if ((instruction & 1U) != 0)
                state.set_condition_field(1,
                    static_cast<std::uint8_t>((state.fpscr >> 28U) & 0xFU));
            break;
        }
        double value = 0.0;
        switch (fp_xo) {
        case 12: value = static_cast<double>(static_cast<float>(a)); break; // frsp
        case 18: value = a / b; break; // fdiv
        case 20: value = a - b; break; // fsub
        case 21: value = a + b; break; // fadd
        case 25: { const unsigned frc = (instruction >> 6U) & 0x1FU;
            value = a * std::bit_cast<double>(state.fpr[frc]); break; } // fmul
        case 28: case 29: case 30: case 31: {
            const unsigned frc = (instruction >> 6U) & 0x1FU;
            const double c = std::bit_cast<double>(state.fpr[frc]);
            const double addend = fp_xo == 28U || fp_xo == 30U ? -b : b;
            value = std::fma(a, c, addend);
            if (fp_xo == 30U || fp_xo == 31U) value = -value;
            break;
        }
        case 23: { // fsel: select FRB for nonnegative FRA, otherwise FRC.
            const unsigned frc = (instruction >> 6U) & 0x1FU;
            value = a >= 0.0 ? b : std::bit_cast<double>(state.fpr[frc]);
            break;
        }
        default: return StepResult::UnsupportedInstruction;
        }
        // Sticky FPSCR summary and primary exception classes.
        if (std::isnan(value) && !std::isnan(a) && !std::isnan(b))
            state.fpscr |= 0xA0000000U; // FX | VX
        if (std::isinf(value) && std::isfinite(a) && std::isfinite(b))
            state.fpscr |= 0x90000000U; // FX | OX
        if (value == 0.0 && a != 0.0 && b != 0.0 &&
            std::isfinite(a) && std::isfinite(b))
            state.fpscr |= 0x88000000U; // FX | UX
        if (fp_xo == 18U && b == 0.0 && a != 0.0)
            state.fpscr |= 0x84000000U; // FX | ZX
        state.fpr[d.rt] = std::bit_cast<std::uint64_t>(value);
        // Floating-point Rc copies FPSCR[0:3] into CR1.
        if ((instruction & 1U) != 0)
            state.set_condition_field(1, static_cast<std::uint8_t>((state.fpscr >> 28U) & 0xFU));
        break;
    }
    case 4: { // VMX/AltiVec three-register arithmetic subset.
        const unsigned vd = d.rt, va = d.ra, vb = d.rb;
        auto& out = state.vr[vd];
        const auto& a = state.vr[va];
        const auto& b = state.vr[vb];
        // VA-form instructions use a six-bit XO and a third vector operand.
        // vsel selects each result bit from VA or VB using the corresponding VC bit.
        const unsigned va_xo = instruction & 0x3FU;
        if (va_xo == 42U) { // vsel
            const unsigned vc = (instruction >> 6U) & 0x1FU;
            const auto& c = state.vr[vc];
            for (unsigned lane = 0; lane < 4; ++lane)
                out.lane[lane] = (a.lane[lane] & ~c.lane[lane]) |
                                 (b.lane[lane] & c.lane[lane]);
            break;
        }
        if (va_xo == 43U) { // vperm
            const unsigned vc = (instruction >> 6U) & 0x1FU;
            const auto& control = state.vr[vc];
            std::array<std::uint8_t,16> source_a{}, source_b{}, selectors{}, result{};
            for (unsigned lane = 0; lane < 4; ++lane) {
                for (unsigned byte = 0; byte < 4; ++byte) {
                    const unsigned shift = 24U - byte * 8U;
                    source_a[lane * 4U + byte] =
                        static_cast<std::uint8_t>(a.lane[lane] >> shift);
                    source_b[lane * 4U + byte] =
                        static_cast<std::uint8_t>(b.lane[lane] >> shift);
                    selectors[lane * 4U + byte] =
                        static_cast<std::uint8_t>(control.lane[lane] >> shift);
                }
            }
            for (unsigned byte = 0; byte < 16; ++byte) {
                const unsigned selector = selectors[byte] & 0x1FU;
                result[byte] = selector < 16U
                    ? source_a[selector] : source_b[selector - 16U];
            }
            for (unsigned lane = 0; lane < 4; ++lane) {
                const unsigned offset = lane * 4U;
                out.lane[lane] =
                    (std::uint32_t(result[offset]) << 24U) |
                    (std::uint32_t(result[offset + 1U]) << 16U) |
                    (std::uint32_t(result[offset + 2U]) << 8U) |
                    std::uint32_t(result[offset + 3U]);
            }
            break;
        }
        // VX-form VMX encodings carry an 11-bit extended opcode including bit 0.
        const unsigned xo = instruction & 0x7FFU;
        if (xo == 0U || xo == 1024U) { // vaddubm / vsububm
            for (unsigned lane = 0; lane < 4; ++lane) {
                std::uint32_t value = 0;
                for (unsigned byte = 0; byte < 4; ++byte) {
                    const unsigned shift = 24U - byte * 8U;
                    const unsigned x = (a.lane[lane] >> shift) & 0xFFU;
                    const unsigned y = (b.lane[lane] >> shift) & 0xFFU;
                    const unsigned z = xo == 0U ? x + y : x - y;
                    value |= (z & 0xFFU) << shift;
                }
                out.lane[lane] = value;
            }
        } else if (xo == 64U || xo == 1088U) { // vadduhm / vsubuhm
            for (unsigned lane = 0; lane < 4; ++lane) {
                const std::uint32_t x = a.lane[lane], y = b.lane[lane];
                const std::uint32_t hi = xo == 64U
                    ? (((x >> 16U) + (y >> 16U)) & 0xFFFFU)
                    : (((x >> 16U) - (y >> 16U)) & 0xFFFFU);
                const std::uint32_t lo = xo == 64U
                    ? (((x & 0xFFFFU) + (y & 0xFFFFU)) & 0xFFFFU)
                    : (((x & 0xFFFFU) - (y & 0xFFFFU)) & 0xFFFFU);
                out.lane[lane] = (hi << 16U) | lo;
            }
        } else if (xo == 128U || xo == 1152U) { // vadduwm / vsubuwm
            for (unsigned lane = 0; lane < 4; ++lane)
                out.lane[lane] = xo == 128U ? a.lane[lane] + b.lane[lane]
                                            : a.lane[lane] - b.lane[lane];
        } else if (xo == 10U || xo == 74U) { // vaddfp / vsubfp
            for (unsigned lane = 0; lane < 4; ++lane) {
                const float x = std::bit_cast<float>(a.lane[lane]);
                const float y = std::bit_cast<float>(b.lane[lane]);
                out.lane[lane] = std::bit_cast<std::uint32_t>(
                    xo == 10U ? x + y : x - y);
            }
        } else if (xo == 524U || xo == 588U || xo == 652U) {
            // vspltb / vsplth / vspltw select one element from VA and
            // broadcast it across the destination vector.
            const unsigned width = xo == 524U ? 8U : (xo == 588U ? 16U : 32U);
            const unsigned element_count = 128U / width;
            const unsigned index = d.rb & (element_count - 1U);
            std::array<std::uint8_t,16> source{};
            for (unsigned lane = 0; lane < 4; ++lane) {
                const auto word = a.lane[lane];
                for (unsigned byte = 0; byte < 4; ++byte)
                    source[lane * 4U + byte] =
                        static_cast<std::uint8_t>(word >> (24U - byte * 8U));
            }
            std::uint32_t packed = 0;
            if (width == 8U) {
                packed = static_cast<std::uint32_t>(source[index]) * 0x01010101U;
            } else if (width == 16U) {
                const unsigned offset = index * 2U;
                const auto value = (static_cast<std::uint32_t>(source[offset]) << 8U) |
                                   source[offset + 1U];
                packed = (value << 16U) | value;
            } else {
                const unsigned offset = index * 4U;
                packed = (static_cast<std::uint32_t>(source[offset]) << 24U) |
                         (static_cast<std::uint32_t>(source[offset + 1U]) << 16U) |
                         (static_cast<std::uint32_t>(source[offset + 2U]) << 8U) |
                         source[offset + 3U];
            }
            out.lane.fill(packed);
        } else if (xo == 780U || xo == 844U || xo == 908U) {
            // vspltisb / vspltish / vspltisw: sign-extend SIMM5 and
            // broadcast it to every byte, halfword, or word element.
            const unsigned width = xo == 780U ? 8U : (xo == 844U ? 16U : 32U);
            const std::int32_t immediate = static_cast<std::int32_t>(
                static_cast<std::int8_t>((d.ra & 0x10U) != 0U
                    ? (d.ra | 0xE0U) : d.ra));
            const std::uint32_t element_mask = width == 32U
                ? 0xFFFFFFFFU : ((std::uint32_t{1} << width) - 1U);
            const std::uint32_t value =
                static_cast<std::uint32_t>(immediate) & element_mask;
            std::uint32_t packed = value;
            if (width == 8U) packed *= 0x01010101U;
            else if (width == 16U) packed = (value << 16U) | value;
            out.lane.fill(packed);
        } else if ((xo & ~1U) == 198U || (xo & ~1U) == 454U ||
                   (xo & ~1U) == 710U) {
            // vcmpeqfp / vcmpgefp / vcmpgtfp; XO bit 0 is the record bit.
            // Unordered NaN comparisons are false.
            const unsigned compare_xo = xo & ~1U;
            bool all_true = true;
            bool all_false = true;
            for (unsigned lane = 0; lane < 4; ++lane) {
                const float lhs = std::bit_cast<float>(a.lane[lane]);
                const float rhs = std::bit_cast<float>(b.lane[lane]);
                const bool match = compare_xo == 198U ? lhs == rhs :
                    (compare_xo == 454U ? lhs >= rhs : lhs > rhs);
                all_true = all_true && match;
                all_false = all_false && !match;
                out.lane[lane] = match ? 0xFFFFFFFFU : 0U;
            }
            if ((xo & 1U) != 0U)
                state.set_condition_field(6, all_true ? 0x8U :
                    (all_false ? 0x2U : 0x0U));
        } else if ((xo & ~1U) == 6U || (xo & ~1U) == 70U ||
                   (xo & ~1U) == 134U || (xo & ~1U) == 518U ||
                   (xo & ~1U) == 582U || (xo & ~1U) == 646U ||
                   (xo & ~1U) == 774U || (xo & ~1U) == 838U ||
                   (xo & ~1U) == 902U) {
            // VMX equality and greater-than compares for byte/halfword/word lanes.
            // XO bit 0 controls CR6 recording and is not part of the operation.
            const unsigned compare_xo = xo & ~1U;
            const unsigned width = (compare_xo == 6U || compare_xo == 518U ||
                                    compare_xo == 774U) ? 8U :
                (compare_xo == 70U || compare_xo == 582U ||
                 compare_xo == 838U) ? 16U : 32U;
            const bool equality = compare_xo == 6U || compare_xo == 70U ||
                                  compare_xo == 134U;
            const bool signed_compare = compare_xo == 774U ||
                compare_xo == 838U || compare_xo == 902U;
            const unsigned elements = 32U / width;
            const std::uint32_t element_mask = width == 32U
                ? 0xFFFFFFFFU : ((std::uint32_t{1} << width) - 1U);
            bool all_true = true;
            bool all_false = true;
            for (unsigned word = 0; word < 4; ++word) {
                std::uint32_t packed = 0;
                for (unsigned element = 0; element < elements; ++element) {
                    const unsigned shift = 32U - width * (element + 1U);
                    const auto lhs = (a.lane[word] >> shift) & element_mask;
                    const auto rhs = (b.lane[word] >> shift) & element_mask;
                    bool match = false;
                    if (equality) {
                        match = lhs == rhs;
                    } else if (signed_compare && width == 8U) {
                        match = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(lhs)) >
                                std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(rhs));
                    } else if (signed_compare && width == 16U) {
                        match = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(lhs)) >
                                std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(rhs));
                    } else if (signed_compare) {
                        match = std::bit_cast<std::int32_t>(lhs) >
                                std::bit_cast<std::int32_t>(rhs);
                    } else {
                        match = lhs > rhs;
                    }
                    all_true = all_true && match;
                    all_false = all_false && !match;
                    if (match) packed |= element_mask << shift;
                }
                out.lane[word] = packed;
            }
            if ((xo & 1U) != 0U)
                state.set_condition_field(6, all_true ? 0x8U :
                    (all_false ? 0x2U : 0x0U));
        } else if (xo == 4U || xo == 68U || xo == 132U ||
                   xo == 260U || xo == 324U || xo == 388U) {
            // VMX rounded averages: unsigned/signed byte, halfword and word.
            const bool signed_lanes = xo >= 260U;
            const unsigned width = (xo == 4U || xo == 260U) ? 8U :
                                   (xo == 68U || xo == 324U) ? 16U : 32U;
            const std::uint64_t lane_mask = width == 32U
                ? 0xFFFFFFFFULL : ((std::uint64_t{1} << width) - 1U);
            const unsigned lanes_per_word = 32U / width;
            for (unsigned word = 0; word < 4; ++word) {
                std::uint32_t packed = 0;
                for (unsigned element = 0; element < lanes_per_word; ++element) {
                    const unsigned shift = 32U - width * (element + 1U);
                    const auto raw_a = (a.lane[word] >> shift) & lane_mask;
                    const auto raw_b = (b.lane[word] >> shift) & lane_mask;
                    std::uint64_t result{};
                    if (signed_lanes) {
                        const std::int64_t x = width == 8U
                            ? std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(raw_a))
                            : width == 16U
                                ? std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(raw_a))
                                : std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw_a));
                        const std::int64_t y = width == 8U
                            ? std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(raw_b))
                            : width == 16U
                                ? std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(raw_b))
                                : std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw_b));
                        const std::int64_t sum = x + y + 1;
                        const std::int64_t average = sum >= 0
                            ? sum / 2 : -(((-sum) + 1) / 2);
                        result = static_cast<std::uint64_t>(average) & lane_mask;
                    } else {
                        result = (raw_a + raw_b + 1U) >> 1U;
                    }
                    packed |= static_cast<std::uint32_t>(result << shift);
                }
                out.lane[word] = packed;
            }
        } else if (xo == 2U || xo == 130U || xo == 258U || xo == 386U) {
            // vminub, vminuw, vminsb and vminsw. Compare lanes at their
            // architectural element width; vector words are stored big-endian.
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (xo == 2U) { // unsigned byte minimum
                    std::uint32_t value = 0;
                    for (unsigned byte = 0; byte < 4; ++byte) {
                        const unsigned shift = 24U - byte * 8U;
                        const auto x = (a.lane[lane] >> shift) & 0xFFU;
                        const auto y = (b.lane[lane] >> shift) & 0xFFU;
                        value |= (x < y ? x : y) << shift;
                    }
                    out.lane[lane] = value;
                } else if (xo == 258U) { // signed byte minimum
                    std::uint32_t value = 0;
                    for (unsigned byte = 0; byte < 4; ++byte) {
                        const unsigned shift = 24U - byte * 8U;
                        const auto x = std::bit_cast<std::int8_t>(
                            static_cast<std::uint8_t>(a.lane[lane] >> shift));
                        const auto y = std::bit_cast<std::int8_t>(
                            static_cast<std::uint8_t>(b.lane[lane] >> shift));
                        const auto minimum = x < y ? x : y;
                        value |= static_cast<std::uint32_t>(
                            std::bit_cast<std::uint8_t>(minimum)) << shift;
                    }
                    out.lane[lane] = value;
                } else if (xo == 130U) { // unsigned word minimum
                    out.lane[lane] = a.lane[lane] < b.lane[lane]
                        ? a.lane[lane] : b.lane[lane];
                } else { // signed word minimum (xo == 386)
                    const auto x = std::bit_cast<std::int32_t>(a.lane[lane]);
                    const auto y = std::bit_cast<std::int32_t>(b.lane[lane]);
                    out.lane[lane] = std::bit_cast<std::uint32_t>(x < y ? x : y);
                }
            }
        } else if (xo == 514U || xo == 642U || xo == 770U || xo == 898U) {
            // vmaxub, vmaxuw, vmaxsb and vmaxsw.
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (xo == 642U) {
                    out.lane[lane] = a.lane[lane] > b.lane[lane]
                        ? a.lane[lane] : b.lane[lane];
                } else if (xo == 898U) {
                    const auto x = std::bit_cast<std::int32_t>(a.lane[lane]);
                    const auto y = std::bit_cast<std::int32_t>(b.lane[lane]);
                    out.lane[lane] = std::bit_cast<std::uint32_t>(x > y ? x : y);
                } else {
                    std::uint32_t value = 0;
                    for (unsigned byte = 0; byte < 4; ++byte) {
                        const unsigned shift = 24U - byte * 8U;
                        const auto ux = static_cast<std::uint8_t>(a.lane[lane] >> shift);
                        const auto uy = static_cast<std::uint8_t>(b.lane[lane] >> shift);
                        if (xo == 514U) {
                            value |= static_cast<std::uint32_t>(ux > uy ? ux : uy) << shift;
                        } else {
                            const auto sx = std::bit_cast<std::int8_t>(ux);
                            const auto sy = std::bit_cast<std::int8_t>(uy);
                            const auto maximum = sx > sy ? sx : sy;
                            value |= static_cast<std::uint32_t>(
                                std::bit_cast<std::uint8_t>(maximum)) << shift;
                        }
                    }
                    out.lane[lane] = value;
                }
            }
        } else if (xo == 1028U || xo == 1092U || xo == 1156U ||
                   xo == 1220U || xo == 1284U) { // vand/vandc/vor/vxor/vnor
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (xo == 1028U) out.lane[lane] = a.lane[lane] & b.lane[lane];
                else if (xo == 1092U) out.lane[lane] = a.lane[lane] & ~b.lane[lane];
                else if (xo == 1156U) out.lane[lane] = a.lane[lane] | b.lane[lane];
                else if (xo == 1220U) out.lane[lane] = a.lane[lane] ^ b.lane[lane];
                else out.lane[lane] = ~(a.lane[lane] | b.lane[lane]);
            }
        } else {
            return StepResult::UnsupportedInstruction;
        }
        break;
    }
    case 31:
        // ISEL's BC field overlaps the upper bits of the ten-bit XO field.
        // Decode its five-bit XO separately before the ordinary X-form switch.
        if ((d.xo & 0x1FU) == 15U) {
            const unsigned condition_bit = (instruction >> 6U) & 0x1FU;
            const bool condition =
                ((state.cr >> (31U - condition_bit)) & 1U) != 0U;
            const std::uint64_t if_true =
                d.ra == 0U ? 0U : state.gpr[d.ra];
            result = condition ? if_true : state.gpr[d.rb];
            destination = d.rt;
            writes_result = true;
            break;
        }
        switch (d.xo) {
        case 103: case 359: case 231: case 487: { // LVX/LVXL/STVX/STVXL.
            const bool store_vector = d.xo == 231U || d.xo == 487U;
            const std::uint64_t effective = (d.ra == 0 ? 0U : state.gpr[d.ra]) +
                                            state.gpr[d.rb];
            const std::uint64_t address = effective & ~std::uint64_t{15};
            std::array<std::uint8_t, 16> bytes{};
            if (!store_vector) {
                if (!read_memory(address, bytes)) {
                    if (fault_address) *fault_address = address;
                    return StepResult::MemoryFault;
                }
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const unsigned offset = lane * 4U;
                    state.vr[d.rt].lane[lane] =
                        (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
                        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
                        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
                        bytes[offset + 3U];
                }
            } else {
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const std::uint32_t value = state.vr[d.rt].lane[lane];
                    const unsigned offset = lane * 4U;
                    bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
                    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 16U);
                    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 8U);
                    bytes[offset + 3U] = static_cast<std::uint8_t>(value);
                }
                if (!write_memory(address, bytes)) {
                    if (fault_address) *fault_address = address;
                    return StepResult::MemoryFault;
                }
            }
            break;
        }
        case 20: { // lwarx: load word and establish a reservation.
            const std::uint64_t address = (d.ra == 0 ? 0U : state.gpr[d.ra]) +
                                          state.gpr[d.rb];
            if ((address & 3U) != 0U) {
                if (fault_address) *fault_address = address;
                return StepResult::AlignmentFault;
            }
            std::array<std::uint8_t, 4> bytes{};
            if (!read_memory(address, bytes)) {
                if (fault_address) *fault_address = address;
                return StepResult::MemoryFault;
            }
            result = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                     (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                     (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
            writes_result = true;
            state.reservation_valid = true;
            state.reservation_address = address;
            break;
        }
        case 150: { // stwcx.: conditional word store; Rc must be set.
            if (!d.record) return StepResult::UnsupportedInstruction;
            const std::uint64_t address = (d.ra == 0 ? 0U : state.gpr[d.ra]) +
                                          state.gpr[d.rb];
            if ((address & 3U) != 0U) {
                if (fault_address) *fault_address = address;
                return StepResult::AlignmentFault;
            }
            const bool success = state.reservation_valid &&
                state.reservation_address == address;
            state.reservation_valid = false;
            if (success) {
                const std::uint32_t value = static_cast<std::uint32_t>(state.gpr[d.rt]);
                const std::array<std::uint8_t, 4> bytes{
                    static_cast<std::uint8_t>(value >> 24U),
                    static_cast<std::uint8_t>(value >> 16U),
                    static_cast<std::uint8_t>(value >> 8U),
                    static_cast<std::uint8_t>(value)};
                if (!write_memory(address, bytes)) {
                    if (fault_address) *fault_address = address;
                    return StepResult::MemoryFault;
                }
            }
            state.set_condition_field(0, static_cast<std::uint8_t>(
                (success ? 0x2U : 0U) | ((state.xer >> 31U) & 1U)));
            break;
        }
        case 23: case 55: case 87: case 119:
        case 279: case 311: case 343: case 375:
        case 151: case 183: case 215: case 247:
        case 407: case 439: case 534: case 662:
        case 790: case 918: { // Indexed integer loads/stores.
            const bool store = d.xo == 151 || d.xo == 183 || d.xo == 215 ||
                               d.xo == 247 || d.xo == 407 || d.xo == 439 || d.xo == 662 || d.xo == 918;
            const bool byte_access = d.xo == 87 || d.xo == 119 ||
                                     d.xo == 215 || d.xo == 247;
            const bool half_access = d.xo == 279 || d.xo == 311 ||
                                     d.xo == 343 || d.xo == 375 || d.xo == 790 ||
                                     d.xo == 407 || d.xo == 439 || d.xo == 918;
            const bool reversed = d.xo == 534 || d.xo == 662 ||
                                  d.xo == 790 || d.xo == 918;
            const bool update = d.xo == 55 || d.xo == 119 || d.xo == 183 ||
                                d.xo == 247 || d.xo == 311 || d.xo == 375 ||
                                d.xo == 439;
            if (update && (d.ra == 0 || (!store && d.ra == d.rt)))
                return StepResult::UnsupportedInstruction;
            const std::size_t width = byte_access ? 1U : (half_access ? 2U : 4U);
            const std::uint64_t address = (d.ra == 0 ? 0U : state.gpr[d.ra]) +
                                          state.gpr[d.rb];
            if (width > 1U && (address & (width - 1U)) != 0U) {
                if (fault_address) *fault_address = address;
                return StepResult::AlignmentFault;
            }
            std::array<std::uint8_t, 4> bytes{};
            if (!store) {
                if (!read_memory(address, std::span<std::uint8_t>(bytes.data(), width))) {
                    if (fault_address) *fault_address = address;
                    return StepResult::MemoryFault;
                }
                if (reversed) std::reverse(bytes.begin(), bytes.begin() + width);
                if (width == 4U) {
                    result = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                             (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                             (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
                } else if (width == 2U) {
                    const std::uint16_t value = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
                    result = (d.xo == 343 || d.xo == 375)
                        ? static_cast<std::uint64_t>(static_cast<std::int64_t>(
                              static_cast<std::int16_t>(value))) : value;
                } else {
                    result = bytes[0];
                }
                writes_result = true;
            } else {
                const std::uint32_t value = static_cast<std::uint32_t>(state.gpr[d.rt]);
                for (std::size_t n = 0; n < width; ++n)
                    bytes[n] = static_cast<std::uint8_t>(value >> ((width - 1U - n) * 8U));
                if (reversed) std::reverse(bytes.begin(), bytes.begin() + width);
                if (!write_memory(address, std::span<const std::uint8_t>(bytes.data(), width))) {
                    if (fault_address) *fault_address = address;
                    return StepResult::MemoryFault;
                }
            }
            if (update) state.gpr[d.ra] = address;
            break;
        }
        case 19: // mfcr: RT = CR
            result = state.cr;
            writes_result = true;
            break;
        case 144: { // mtcrf: copy selected CR fields from RS
            const unsigned field_mask = (instruction >> 12U) & 0xFFU;
            const std::uint32_t source = static_cast<std::uint32_t>(state.gpr[d.rt]);
            for (unsigned field = 0; field < 8; ++field) {
                if ((field_mask & (1U << (7U - field))) != 0) {
                    const unsigned shift = 28U - field * 4U;
                    state.set_condition_field(field, static_cast<std::uint8_t>((source >> shift) & 0xFU));
                }
            }
            break;
        }
        case 595: { // mfsr: read one of the sixteen segment registers.
            constexpr std::uint64_t msr_pr = std::uint64_t{1} << 14U;
            if ((state.msr & msr_pr) != 0) return StepResult::ProgramException;
            const unsigned sr = (instruction >> 16U) & 0xFU;
            result = state.segment_registers[sr];
            writes_result = true;
            break;
        }
        case 210: { // mtsr: write one of the sixteen segment registers.
            constexpr std::uint64_t msr_pr = std::uint64_t{1} << 14U;
            if ((state.msr & msr_pr) != 0) return StepResult::ProgramException;
            const unsigned sr = (instruction >> 16U) & 0xFU;
            state.segment_registers[sr] = static_cast<std::uint32_t>(state.gpr[d.rt]);
            break;
        }
        case 83: { // mfmsr: privileged read of the machine state register.
            constexpr std::uint64_t msr_pr = std::uint64_t{1} << 14U;
            if ((state.msr & msr_pr) != 0) return StepResult::ProgramException;
            result = static_cast<std::uint32_t>(state.msr);
            writes_result = true;
            break;
        }
        case 146: { // mtmsr: privileged write of the 32-bit machine state register.
            constexpr std::uint64_t msr_pr = std::uint64_t{1} << 14U;
            if ((state.msr & msr_pr) != 0) return StepResult::ProgramException;
            state.msr = static_cast<std::uint32_t>(state.gpr[d.rt]);
            break;
        }
        case 339: { // mfspr: supported Xenon-visible SPRs
            const unsigned spr = (((instruction >> 11U) & 0x1FU) << 5U) |
                                 ((instruction >> 16U) & 0x1FU);
            switch (spr) {
            case 1: result = state.xer; break; // XER
            case 8: result = state.lr; break;  // LR
            case 9: result = state.ctr; break; // CTR
            default: return StepResult::UnsupportedInstruction;
            }
            writes_result = true;
            break;
        }
        case 371: { // mftb: read the Xenon time-base register (TBR).
            const unsigned tbr = (((instruction >> 11U) & 0x1FU) << 5U) |
                                 ((instruction >> 16U) & 0x1FU);
            switch (tbr) {
            case 268: result = state.read_timebase_lower(); break; // TBL
            case 269: result = state.read_timebase_upper(); break; // TBU
            default: return StepResult::UnsupportedInstruction;
            }
            writes_result = true;
            break;
        }
        case 512: { // mcrxr: move XER[SO,OV,CA] into a CR field and clear them.
            const unsigned bf = (instruction >> 23U) & 7U;
            constexpr std::uint64_t xer_so_ov_ca =
                (std::uint64_t{1} << 31U) |
                (std::uint64_t{1} << 30U) |
                (std::uint64_t{1} << 29U);
            const std::uint8_t field = static_cast<std::uint8_t>(
                ((state.xer >> 31U) & 1U) << 3U |
                ((state.xer >> 30U) & 1U) << 2U |
                ((state.xer >> 29U) & 1U) << 1U);
            state.set_condition_field(bf, field);
            state.xer &= ~xer_so_ov_ca;
            break;
        }
        case 467: { // mtspr: supported Xenon-visible SPRs
            const unsigned spr = (((instruction >> 11U) & 0x1FU) << 5U) |
                                 ((instruction >> 16U) & 0x1FU);
            const std::uint64_t source = state.gpr[d.rt];
            switch (spr) {
            case 1: state.xer = source; break; // XER
            case 8: state.lr = source; break;  // LR
            case 9: state.ctr = source; break; // CTR
            case 284: // TBL (write alias)
            case 285: { // TBU (write alias)
                constexpr std::uint64_t msr_pr = std::uint64_t{1} << 14U;
                if ((state.msr & msr_pr) != 0) return StepResult::ProgramException;
                if (spr == 284U) state.write_timebase_lower(static_cast<std::uint32_t>(source));
                else state.write_timebase_upper(static_cast<std::uint32_t>(source));
                break;
            }
            default: return StepResult::UnsupportedInstruction;
            }
            break;
        }
        case 0: { // cmp / cmpd / cmpw
            const unsigned bf = (instruction >> 23U) & 7U;
            const bool is_64 = ((instruction >> 21U) & 1U) != 0;
            const std::uint64_t lhs = is_64 ? state.gpr[d.ra] :
                static_cast<std::uint32_t>(state.gpr[d.ra]);
            const std::uint64_t rhs = is_64 ? state.gpr[d.rb] :
                static_cast<std::uint32_t>(state.gpr[d.rb]);
            const bool less = is_64
                ? static_cast<std::int64_t>(lhs) < static_cast<std::int64_t>(rhs)
                : static_cast<std::int32_t>(lhs) < static_cast<std::int32_t>(rhs);
            const bool greater = is_64
                ? static_cast<std::int64_t>(lhs) > static_cast<std::int64_t>(rhs)
                : static_cast<std::int32_t>(lhs) > static_cast<std::int32_t>(rhs);
            std::uint8_t field = less ? 0x8U : (greater ? 0x4U : 0x2U);
            if ((state.xer & (std::uint64_t{1} << 31U)) != 0) field |= 0x1U;
            state.set_condition_field(bf, field);
            break;
        }
        case 32: { // cmpl / cmpld / cmplw
            const unsigned bf = (instruction >> 23U) & 7U;
            const bool is_64 = ((instruction >> 21U) & 1U) != 0;
            const std::uint64_t lhs = is_64 ? state.gpr[d.ra] :
                static_cast<std::uint32_t>(state.gpr[d.ra]);
            const std::uint64_t rhs = is_64 ? state.gpr[d.rb] :
                static_cast<std::uint32_t>(state.gpr[d.rb]);
            const std::uint8_t field = lhs < rhs ? 0x8U : (lhs > rhs ? 0x4U : 0x2U);
            state.set_condition_field(bf, field | ((state.xer >> 31U) & 1U));
            break;
        }
        case 24: { // slw: shift left word; RB[5] forces zero
            const std::uint32_t value = static_cast<std::uint32_t>(state.gpr[d.rt]);
            const unsigned shift = static_cast<unsigned>(state.gpr[d.rb] & 0x3FU);
            result = (shift & 0x20U) != 0 ? 0U : static_cast<std::uint32_t>(value << shift);
            destination = d.ra;
            writes_result = true;
            record_32bit = true;
            break;
        }
        case 536: { // srw: logical shift right word; RB[5] forces zero
            const std::uint32_t value = static_cast<std::uint32_t>(state.gpr[d.rt]);
            const unsigned shift = static_cast<unsigned>(state.gpr[d.rb] & 0x3FU);
            result = (shift & 0x20U) != 0 ? 0U : (value >> shift);
            destination = d.ra;
            writes_result = true;
            record_32bit = true;
            break;
        }
        case 792: case 824: { // sraw / srawi: arithmetic shift right word
            const std::uint32_t raw = static_cast<std::uint32_t>(state.gpr[d.rt]);
            const std::int32_t value = static_cast<std::int32_t>(raw);
            const unsigned shift = d.xo == 792
                ? static_cast<unsigned>(state.gpr[d.rb] & 0x3FU)
                : static_cast<unsigned>((instruction >> 11U) & 0x1FU);
            const bool negative = value < 0;
            bool shifted_out_one = false;
            if (shift == 0) {
                result = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
            } else if (shift >= 32U) {
                result = negative ? UINT64_MAX : 0U;
                shifted_out_one = negative && (raw & 0x7FFFFFFFU) != 0;
            } else {
                const std::uint32_t discarded_mask = (std::uint32_t{1} << shift) - 1U;
                shifted_out_one = negative && (raw & discarded_mask) != 0;
                result = static_cast<std::uint64_t>(static_cast<std::int64_t>(value >> shift));
            }
            constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
            state.xer = shifted_out_one ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
            destination = d.ra;
            writes_result = true;
            record_32bit = true;
            break;
        }
        case 27: { // sld: shift left doubleword; RB[5:6] force zero
            const unsigned shift = static_cast<unsigned>(state.gpr[d.rb] & 0x7FU);
            result = shift >= 64U ? 0U : (state.gpr[d.rt] << shift);
            destination = d.ra;
            writes_result = true;
            break;
        }
        case 539: { // srd: logical shift right doubleword
            const unsigned shift = static_cast<unsigned>(state.gpr[d.rb] & 0x7FU);
            result = shift >= 64U ? 0U : (state.gpr[d.rt] >> shift);
            destination = d.ra;
            writes_result = true;
            break;
        }
        case 794: { // srad: arithmetic shift right doubleword
            const std::uint64_t value = state.gpr[d.rt];
            const unsigned shift = static_cast<unsigned>(state.gpr[d.rb] & 0x7FU);
            const bool negative = (value >> 63U) != 0;
            bool shifted_out_one = false;
            if (shift == 0U) {
                result = value;
            } else if (shift >= 64U) {
                result = negative ? UINT64_MAX : 0U;
                shifted_out_one = negative && (value & 0x7FFFFFFFFFFFFFFFULL) != 0;
            } else {
                const std::uint64_t discarded_mask = (std::uint64_t{1} << shift) - 1U;
                shifted_out_one = negative && (value & discarded_mask) != 0;
                result = value >> shift;
                if (negative) result |= (UINT64_MAX << (64U - shift));
            }
            constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
            state.xer = shifted_out_one ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
            destination = d.ra;
            writes_result = true;
            break;
        }
        case 26: { // cntlzw: count leading zero bits in the low 32-bit word
            const std::uint32_t value = static_cast<std::uint32_t>(state.gpr[d.rt]);
            unsigned count = 0;
            while (count < 32U && (value & (std::uint32_t{1} << (31U - count))) == 0)
                ++count;
            result = count;
            destination = d.ra;
            writes_result = true;
            record_32bit = true;
            break;
        }
        case 28:  // and
            result = state.gpr[d.rt] & state.gpr[d.rb];
            destination = d.ra;
            writes_result = true;
            break;
        case 60: // andc
            result = state.gpr[d.rt] & ~state.gpr[d.rb];
            destination = d.ra;
            writes_result = true;
            break;
        case 124: // nor
            result = ~(state.gpr[d.rt] | state.gpr[d.rb]);
            destination = d.ra;
            writes_result = true;
            break;
        case 284: // eqv
            result = ~(state.gpr[d.rt] ^ state.gpr[d.rb]);
            destination = d.ra;
            writes_result = true;
            break;
        case 412: // orc
            result = state.gpr[d.rt] | ~state.gpr[d.rb];
            destination = d.ra;
            writes_result = true;
            break;
        case 476: // nand
            result = ~(state.gpr[d.rt] & state.gpr[d.rb]);
            destination = d.ra;
            writes_result = true;
            break;
        case 954: // extsb
            result = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(static_cast<std::int8_t>(state.gpr[d.rt])));
            destination = d.ra;
            writes_result = true;
            break;
        case 922: // extsh
            result = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(static_cast<std::int16_t>(state.gpr[d.rt])));
            destination = d.ra;
            writes_result = true;
            break;
        case 986: // extsw
            result = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(static_cast<std::int32_t>(state.gpr[d.rt])));
            destination = d.ra;
            writes_result = true;
            break;
        case 122: { // popcntb: count set bits independently in each byte.
            const auto value = state.gpr[d.rt];
            result = 0;
            for (unsigned byte = 0; byte < 8U; ++byte) {
                const auto lane = static_cast<std::uint8_t>(value >> (byte * 8U));
                result |= static_cast<std::uint64_t>(std::popcount(lane)) << (byte * 8U);
            }
            destination = d.ra;
            writes_result = true;
            break;
        }
        case 378: // popcntw: count set bits in the low 32-bit word.
            result = std::popcount(static_cast<std::uint32_t>(state.gpr[d.rt]));
            destination = d.ra;
            writes_result = true;
            break;
        case 506: // popcntd: count set bits in the full doubleword.
            result = std::popcount(state.gpr[d.rt]);
            destination = d.ra;
            writes_result = true;
            break;
        case 58: { // cntlzd
            const std::uint64_t value = state.gpr[d.rt];
            unsigned count = 0;
            while (count < 64U && (value & (std::uint64_t{1} << (63U - count))) == 0)
                ++count;
            result = count;
            destination = d.ra;
            writes_result = true;
            break;
        }
        case 104: { // neg (OE=0 only): RT = -RA
            if ((instruction & (1U << 10U)) != 0) return StepResult::UnsupportedInstruction;
            result = std::uint64_t{0} - state.gpr[d.ra];
            destination = d.rt;
            writes_result = true;
            break;
        }
        case 138: { // adde (OE=0 only): RA + RB + XER[CA]
            if ((instruction & (1U << 10U)) != 0) return StepResult::UnsupportedInstruction;
            const std::uint64_t lhs = state.gpr[d.ra];
            const std::uint64_t rhs = state.gpr[d.rb];
            const std::uint64_t carry_in = (state.xer >> 29U) & 1U;
            const std::uint64_t partial = lhs + rhs;
            const bool carry1 = partial < lhs;
            result = partial + carry_in;
            const bool carry2 = result < partial;
            constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
            state.xer = (carry1 || carry2) ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
            destination = d.rt;
            writes_result = true;
            break;
        }
        case 136: { // subfe (OE=0 only): ~RA + RB + XER[CA]
            if ((instruction & (1U << 10U)) != 0) return StepResult::UnsupportedInstruction;
            const std::uint64_t lhs = ~state.gpr[d.ra];
            const std::uint64_t rhs = state.gpr[d.rb];
            const std::uint64_t carry_in = (state.xer >> 29U) & 1U;
            const std::uint64_t partial = lhs + rhs;
            const bool carry1 = partial < lhs;
            result = partial + carry_in;
            const bool carry2 = result < partial;
            constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
            state.xer = (carry1 || carry2) ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
            destination = d.rt;
            writes_result = true;
            break;
        }
        case 202: { // addze (OE=0 only): RT = RA + XER[CA]
            if ((instruction & (1U << 10U)) != 0) return StepResult::UnsupportedInstruction;
            const std::uint64_t lhs = state.gpr[d.ra];
            const std::uint64_t carry_in = (state.xer >> 29U) & 1U;
            result = lhs + carry_in;
            constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
            state.xer = (carry_in != 0 && result == 0)
                ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
            destination = d.rt;
            writes_result = true;
            break;
        }
        case 200: { // subfze (OE=0 only): RT = ~RA + XER[CA]
            if ((instruction & (1U << 10U)) != 0) return StepResult::UnsupportedInstruction;
            const std::uint64_t lhs = ~state.gpr[d.ra];
            const std::uint64_t carry_in = (state.xer >> 29U) & 1U;
            result = lhs + carry_in;
            constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
            state.xer = (carry_in != 0 && result == 0)
                ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
            destination = d.rt;
            writes_result = true;
            break;
        }
        case 234: case 232: { // addme / subfme (OE=0 only): lhs + XER[CA] - 1
            if ((instruction & (1U << 10U)) != 0) return StepResult::UnsupportedInstruction;
            const std::uint64_t lhs = d.xo == 234 ? state.gpr[d.ra] : ~state.gpr[d.ra];
            const std::uint64_t carry_in = (state.xer >> 29U) & 1U;
            const std::uint64_t partial = lhs + UINT64_MAX;
            const bool carry1 = partial < lhs;
            result = partial + carry_in;
            const bool carry2 = result < partial;
            constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
            state.xer = (carry1 || carry2) ? (state.xer | xer_ca) : (state.xer & ~xer_ca);
            destination = d.rt;
            writes_result = true;
            break;
        }
        case 235: case 747: { // mullw: signed 32-bit product; OE updates XER[OV,SO].
            const auto lhs = static_cast<std::int32_t>(state.gpr[d.ra]);
            const auto rhs = static_cast<std::int32_t>(state.gpr[d.rb]);
            const std::int64_t product = static_cast<std::int64_t>(lhs) * rhs;
            const std::uint32_t low_product = static_cast<std::uint32_t>(product);
            result = static_cast<std::uint64_t>(static_cast<std::int64_t>(
                static_cast<std::int32_t>(low_product)));
            if ((instruction & (1U << 10U)) != 0) {
                constexpr std::uint64_t xer_ov = std::uint64_t{1} << 30U;
                constexpr std::uint64_t xer_so = std::uint64_t{1} << 31U;
                const bool overflow = product < INT32_MIN || product > INT32_MAX;
                state.xer = overflow ? (state.xer | xer_ov | xer_so)
                                     : (state.xer & ~xer_ov);
            }
            destination = d.rt;
            writes_result = true;
            record_32bit = true;
            break;
        }
        case 491: case 1003: { // divw: signed word division; OE reports divide-by-zero and signed overflow.
            const auto dividend = static_cast<std::int32_t>(state.gpr[d.ra]);
            const auto divisor = static_cast<std::int32_t>(state.gpr[d.rb]);
            const bool overflow = divisor == 0 || (dividend == INT32_MIN && divisor == -1);
            std::int32_t quotient = 0;
            if (!overflow) quotient = static_cast<std::int32_t>(dividend / divisor);
            if ((instruction & (1U << 10U)) != 0) {
                constexpr std::uint64_t xer_ov = std::uint64_t{1} << 30U;
                constexpr std::uint64_t xer_so = std::uint64_t{1} << 31U;
                state.xer = overflow ? (state.xer | xer_ov | xer_so)
                                     : (state.xer & ~xer_ov);
            }
            result = static_cast<std::uint64_t>(static_cast<std::int64_t>(quotient));
            destination = d.rt;
            writes_result = true;
            record_32bit = true;
            break;
        }
        case 459: case 971: { // divwu: unsigned word division; OE reports divide-by-zero.
            const auto dividend = static_cast<std::uint32_t>(state.gpr[d.ra]);
            const auto divisor = static_cast<std::uint32_t>(state.gpr[d.rb]);
            const bool overflow = divisor == 0U;
            const std::uint32_t quotient = overflow ? 0U : dividend / divisor;
            if (d.xo == 971U) {
                constexpr std::uint64_t xer_ov = std::uint64_t{1} << 30U;
                constexpr std::uint64_t xer_so = std::uint64_t{1} << 31U;
                state.xer = overflow ? (state.xer | xer_ov | xer_so)
                                     : (state.xer & ~xer_ov);
            }
            result = quotient;
            destination = d.rt;
            writes_result = true;
            record_32bit = true;
            break;
        }
        case 266: case 778: { // add: OE updates XER[OV,SO] using signed 64-bit overflow.
            const std::uint64_t lhs = state.gpr[d.ra];
            const std::uint64_t rhs = state.gpr[d.rb];
            result = lhs + rhs;
            if (d.xo == 778U) {
                constexpr std::uint64_t xer_ov = std::uint64_t{1} << 30U;
                constexpr std::uint64_t xer_so = std::uint64_t{1} << 31U;
                const bool overflow = ((~(lhs ^ rhs) & (lhs ^ result)) >> 63U) != 0;
                state.xer = overflow ? (state.xer | xer_ov | xer_so)
                                     : (state.xer & ~xer_ov);
            }
            destination = d.rt;
            writes_result = true;
            break;
        }
        case 40: case 552: { // subf: RB - RA; OE updates signed 64-bit overflow.
            const std::uint64_t lhs = state.gpr[d.rb];
            const std::uint64_t rhs = state.gpr[d.ra];
            result = lhs - rhs;
            if (d.xo == 552U) {
                constexpr std::uint64_t xer_ov = std::uint64_t{1} << 30U;
                constexpr std::uint64_t xer_so = std::uint64_t{1} << 31U;
                const bool overflow = (((lhs ^ rhs) & (lhs ^ result)) >> 63U) != 0;
                state.xer = overflow ? (state.xer | xer_ov | xer_so)
                                     : (state.xer & ~xer_ov);
            }
            destination = d.rt;
            writes_result = true;
            break;
        }
        case 316: // xor
            result = state.gpr[d.rt] ^ state.gpr[d.rb];
            destination = d.ra;
            writes_result = true;
            break;
        case 444: // or
            result = state.gpr[d.rt] | state.gpr[d.rb];
            destination = d.ra;
            writes_result = true;
            break;
        default:
            return StepResult::UnsupportedInstruction;
        }
        break;
    default:
        return StepResult::UnsupportedInstruction;
    }

    if (writes_result) {
        state.gpr[destination] = result;

        if (d.record) {
            update_cr0(state, result, record_32bit ? (std::uint64_t{1} << 31U)
                                                   : (std::uint64_t{1} << 63U));
        }
    }

    if (update_base) state.gpr[d.ra] = updated_address;
    state.pc = branch_taken ? branch_target : state.pc + 4U;
    return StepResult::Executed;
}

// Fetch one big-endian Xenon instruction through the guest virtual address
// space. Execute permission is checked independently from data-read permission.
[[nodiscard]] inline StepResult fetch_virtual_instruction(
    std::uint64_t guest_pc, const MemoryAccess& bus,
    std::uint32_t& instruction) noexcept {
    if (!bus.fetch) return StepResult::MemoryFault;
    std::array<std::uint8_t,4> bytes{};
    if (!bus.fetch(bus.context,guest_pc,bytes)) return StepResult::MemoryFault;
    instruction=(static_cast<std::uint32_t>(bytes[0])<<24U)|
        (static_cast<std::uint32_t>(bytes[1])<<16U)|
        (static_cast<std::uint32_t>(bytes[2])<<8U)|bytes[3];
    return StepResult::Executed;
}

// Architectural exception delivery wrapper for the interpreter. The low-level
// execute() API remains useful for tests/debuggers that want fault results
// without changing PC; guest execution should use this wrapper to enter the
// PowerPC data-storage (0x300) or alignment (0x600) exception vector.
[[nodiscard]] inline StepResult execute_with_exception_delivery(
    XenonThreadState& state, std::uint32_t instruction,
    std::span<std::uint8_t> memory = {},
    std::uint64_t* fault_address = nullptr,
    const MemoryAccess* bus = nullptr) noexcept {
    std::uint64_t local_fault = 0;
    auto* fault = fault_address ? fault_address : &local_fault;
    const auto result = execute(state, instruction, memory, fault, bus);
    if (result == StepResult::MemoryFault)
        state.raise_exception(0x300U, *fault);
    else if (result == StepResult::AlignmentFault)
        state.raise_exception(0x600U, *fault);
    else if (result == StepResult::ProgramException)
        state.raise_exception(0x700U);
    return result;
}

[[nodiscard]] inline StepResult fetch_and_execute_with_exception_delivery(
    XenonThreadState& state, const MemoryAccess& bus) noexcept {
    std::uint32_t instruction=0;
    const auto fetched=fetch_virtual_instruction(state.pc,bus,instruction);
    if (fetched!=StepResult::Executed) {
        state.raise_exception(0x400U,state.pc);
        return StepResult::MemoryFault;
    }
    return execute_with_exception_delivery(state,instruction,{},nullptr,&bus);
}


} // namespace onix360::cpu
