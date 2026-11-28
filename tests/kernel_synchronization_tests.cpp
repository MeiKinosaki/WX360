#include "onix360/kernel/kernel_synchronization.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    using namespace onix360::kernel;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
    };

    KernelSynchronization sync;
    const auto manual = sync.create_event(true, false);
    const auto automatic = sync.create_event(false, false);
    const auto semaphore = sync.create_semaphore(1, 3);
    check(manual && automatic && semaphore, "creates event and semaphore objects");
    check(sync.wait_event(*manual, 10) == WaitResult::Blocked &&
          sync.wait_event(*manual, 11) == WaitResult::Blocked,
          "queues multiple waiters on unsignaled manual event");
    const auto manual_woken = sync.set_event(*manual);
    check(manual_woken.size() == 2 && manual_woken[0] == 10 &&
          manual_woken[1] == 11, "manual event releases every waiter FIFO");
    check(sync.wait_event(*manual, 12) == WaitResult::Acquired &&
          sync.reset_event(*manual) &&
          sync.wait_event(*manual, 12) == WaitResult::Blocked,
          "manual event remains signaled until reset");

    sync.cancel_thread(12);
    check(sync.set_event(*manual).empty(),
          "terminated guest thread is removed from event wait queue");

    check(sync.wait_event(*automatic, 20) == WaitResult::Blocked &&
          sync.wait_event(*automatic, 21) == WaitResult::Blocked,
          "queues waiters on auto-reset event");
    const auto auto_woken = sync.set_event(*automatic);
    check(auto_woken.size() == 1 && auto_woken[0] == 20 &&
          sync.set_event(*automatic).size() == 1,
          "auto-reset event releases one waiter per signal");

    check(sync.wait_semaphore(*semaphore, 30) == WaitResult::Acquired &&
          sync.semaphore_count(*semaphore) == 0,
          "wait consumes an available semaphore count");
    check(sync.wait_semaphore(*semaphore, 31) == WaitResult::Blocked &&
          sync.wait_semaphore(*semaphore, 32) == WaitResult::Blocked,
          "queues semaphore waiters when count is zero");
    const auto released = sync.release_semaphore(*semaphore, 2);
    check(released && released->size() == 2 && (*released)[0] == 31 &&
          (*released)[1] == 32 && sync.semaphore_count(*semaphore) == 0,
          "semaphore release wakes FIFO waiters without inflating count");
    check(!sync.release_semaphore(*semaphore, 4),
          "rejects semaphore release beyond maximum count");
    check(sync.wait_event(999, 1) == WaitResult::InvalidObject,
          "rejects unknown synchronization object");
    check(sync.event_waiter_count(*manual) == 0U &&
          sync.event_signaled(*manual) == true,
          "reports manual event signal and waiter state");
    check(sync.semaphore_waiter_count(*semaphore) == 0U,
          "reports semaphore waiter count after wakeup");
    check(sync.close_event(*manual) && !sync.has_event(*manual) &&
          !sync.close_event(*manual),
          "closes event objects exactly once");
    check(sync.close_semaphore(*semaphore) &&
          !sync.semaphore_count(*semaphore) &&
          !sync.close_semaphore(*semaphore),
          "closes semaphore objects exactly once");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
