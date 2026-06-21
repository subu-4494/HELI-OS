#pragma once

#include "task.hpp"
#include <vector>
#include <memory>

namespace helios {

// ─── ContextSwitcher ────────────────────────────────────────────────────────
// Owns the mechanics of context switching between tasks.
// The scheduler decides WHICH task to run next.
// ContextSwitcher handles HOW to actually perform the switch.
//
// Key design decision:
//   makecontext() only accepts a plain void(*)() function pointer.
//   We can't pass a Task* to it as an argument portably on all architectures.
//   Instead, we use a thread-local pointer (current_task) as a trampoline:
//     makecontext → task_entry_trampoline() → reads current_task → calls fn()
//
class ContextSwitcher {
public:
    ContextSwitcher();
    ~ContextSwitcher() = default;

    // Prepare a task's ucontext so it's ready to be switched into.
    // Must be called once per task before the first switch_to().
    // Sets up:
    //   ctx.uc_stack  → points to the task's private heap stack
    //   ctx.uc_link   → where to go when the task function returns (idle ctx)
    //   makecontext   → wires the context entry point to task_entry_trampoline
    void init_task(Task& task);

    // Perform a context switch: save 'from', restore 'to'.
    // After this call returns (which may be much later), 'from' is running again.
    void switch_to(Task* from, Task* to);

    // Switch into 'to' from the bootstrap context (first switch only).
    // Used to enter the first task from main() without a "from" task.
    void enter(Task* to);

    // Currently executing task — read by task_entry_trampoline().
    // thread_local so it's safe if you later add multi-threaded schedulers.
    static thread_local Task* current_task;

private:
    // The idle context: when a task's function returns naturally,
    // uc_link points here, which gracefully exits the scheduler loop.
    ucontext_t idle_ctx_;

    // Trampoline: the actual function makecontext calls.
    // Reads current_task and invokes its fn(), then marks it DONE.
    static void task_entry_trampoline();
};

}  // namespace helios