#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace onix360::kernel {

enum class WaitResult : std::uint8_t { Acquired, Blocked, InvalidObject };

// Deterministic guest synchronization objects. They coordinate guest thread
// IDs only; they do not create host mutexes, events, or OS scheduler threads.
class KernelSynchronization {
public:
    [[nodiscard]] std::optional<std::uint32_t> create_event(
        bool manual_reset, bool initially_signaled = false) {
        if (next_id_ == 0xFFFFFFFFU) return std::nullopt;
        const auto id = next_id_++;
        events_.emplace(id, Event{manual_reset, initially_signaled, {}});
        return id;
    }

    [[nodiscard]] std::optional<std::uint32_t> create_semaphore(
        std::uint32_t initial_count, std::uint32_t maximum_count) {
        if (maximum_count == 0 || initial_count > maximum_count ||
            next_id_ == 0xFFFFFFFFU) return std::nullopt;
        const auto id = next_id_++;
        semaphores_.emplace(id, Semaphore{initial_count, maximum_count, {}});
        return id;
    }

    [[nodiscard]] WaitResult wait_event(std::uint32_t id, std::uint32_t thread_id) {
        auto it = events_.find(id);
        if (it == events_.end() || thread_id == 0) return WaitResult::InvalidObject;
        auto& event = it->second;
        if (event.signaled) {
            if (!event.manual_reset) event.signaled = false;
            return WaitResult::Acquired;
        }
        if (std::find(event.waiters.begin(), event.waiters.end(), thread_id) ==
            event.waiters.end()) event.waiters.push_back(thread_id);
        return WaitResult::Blocked;
    }

    [[nodiscard]] std::vector<std::uint32_t> set_event(std::uint32_t id) {
        std::vector<std::uint32_t> released;
        auto it = events_.find(id);
        if (it == events_.end()) return released;
        auto& event = it->second;
        if (event.manual_reset) {
            event.signaled = true;
            released.assign(event.waiters.begin(), event.waiters.end());
            event.waiters.clear();
        } else if (!event.waiters.empty()) {
            released.push_back(event.waiters.front());
            event.waiters.pop_front();
            event.signaled = false;
        } else {
            event.signaled = true;
        }
        return released;
    }

    [[nodiscard]] bool has_event(std::uint32_t id) const noexcept {
        return events_.contains(id);
    }

    [[nodiscard]] std::optional<bool> event_signaled(std::uint32_t id) const noexcept {
        const auto it = events_.find(id);
        if (it == events_.end()) return std::nullopt;
        return it->second.signaled;
    }

    [[nodiscard]] std::optional<std::size_t> event_waiter_count(
        std::uint32_t id) const noexcept {
        const auto it = events_.find(id);
        if (it == events_.end()) return std::nullopt;
        return it->second.waiters.size();
    }

    [[nodiscard]] std::optional<std::size_t> semaphore_waiter_count(
        std::uint32_t id) const noexcept {
        const auto it = semaphores_.find(id);
        if (it == semaphores_.end()) return std::nullopt;
        return it->second.waiters.size();
    }

    [[nodiscard]] bool close_event(std::uint32_t id) noexcept {
        return events_.erase(id) == 1U;
    }

    [[nodiscard]] bool close_semaphore(std::uint32_t id) noexcept {
        return semaphores_.erase(id) == 1U;
    }

    [[nodiscard]] bool reset_event(std::uint32_t id) noexcept {
        auto it = events_.find(id);
        if (it == events_.end()) return false;
        it->second.signaled = false;
        return true;
    }

    [[nodiscard]] WaitResult wait_semaphore(std::uint32_t id, std::uint32_t thread_id) {
        auto it = semaphores_.find(id);
        if (it == semaphores_.end() || thread_id == 0) return WaitResult::InvalidObject;
        auto& semaphore = it->second;
        if (semaphore.count != 0) {
            --semaphore.count;
            return WaitResult::Acquired;
        }
        if (std::find(semaphore.waiters.begin(), semaphore.waiters.end(), thread_id) ==
            semaphore.waiters.end()) semaphore.waiters.push_back(thread_id);
        return WaitResult::Blocked;
    }

    [[nodiscard]] std::optional<std::vector<std::uint32_t>> release_semaphore(
        std::uint32_t id, std::uint32_t release_count = 1) {
        auto it = semaphores_.find(id);
        if (it == semaphores_.end() || release_count == 0) return std::nullopt;
        auto& semaphore = it->second;
        if (release_count > semaphore.maximum_count - semaphore.count)
            return std::nullopt;
        std::vector<std::uint32_t> released;
        for (std::uint32_t n = 0; n < release_count; ++n) {
            if (!semaphore.waiters.empty()) {
                released.push_back(semaphore.waiters.front());
                semaphore.waiters.pop_front();
            } else {
                ++semaphore.count;
            }
        }
        return released;
    }

    void cancel_thread(std::uint32_t thread_id) noexcept {
        for (auto& [id, event] : events_) {
            (void)id;
            event.waiters.erase(std::remove(event.waiters.begin(), event.waiters.end(), thread_id),
                                event.waiters.end());
        }
        for (auto& [id, semaphore] : semaphores_) {
            (void)id;
            semaphore.waiters.erase(
                std::remove(semaphore.waiters.begin(), semaphore.waiters.end(), thread_id),
                semaphore.waiters.end());
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> semaphore_count(std::uint32_t id) const noexcept {
        const auto it = semaphores_.find(id);
        if (it == semaphores_.end()) return std::nullopt;
        return it->second.count;
    }

private:
    struct Event {
        bool manual_reset{};
        bool signaled{};
        std::deque<std::uint32_t> waiters;
    };
    struct Semaphore {
        std::uint32_t count{};
        std::uint32_t maximum_count{};
        std::deque<std::uint32_t> waiters;
    };
    std::uint32_t next_id_{1};
    std::map<std::uint32_t, Event> events_;
    std::map<std::uint32_t, Semaphore> semaphores_;
};

} // namespace onix360::kernel
