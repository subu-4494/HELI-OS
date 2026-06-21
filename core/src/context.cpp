#include "context.hpp"

#include <cstring>
#include <stdexcept>
#include <cstdio>

namespace helios {

// ─── Thread-local current task pointer ──────────────────────────────────────
// task_entry_trampoline() needs to know which Task it was launched for,
// but makecontext() can only call a void(*)() — no arguments on all archs.
// Solution: store the Task* here before every switch_to(), read it in trampoline.
thread_local Task* ContextSwitcher::current_task = nullptr;

// ─── Constructor ─────────────────────────────────────────────────────────────
ContextSwitcher::ContextSwitcher() {
    // Initialise the idle context.
    // This is where a task lands after its function returns (via uc_link).
    // It's also where enter() returns to when the scheduler wants to stop.
    // We give it a small stack so getcontext doesn't complain.
    //
    // We don't makecontext it — the idle context is just the "fall-through"
    // destination; the CPU will simply continue past the swapcontext call
    // in enter() once all tasks are done.
    if (getcontext(&idle_ctx_) == -1) {
        throw std::runtime_error("getcontext failed for idle context");
    }
}

// ─── init_task ───────────────────────────────────────────────────────────────
// Wire up a Task's ucontext_t so it can be switched into.
//
// Step-by-step what happens here:
//   1. getcontext()   — snapshot current CPU state into task.ctx as a baseline
//   2. uc_stack       — redirect the context to use the task's private stack
//   3. uc_link        — when task.fn() returns, resume idle_ctx_ (graceful exit)
//   4. makecontext()  — overwrite the instruction pointer in task.ctx to point
//                       at task_entry_trampoline, so the first switch_to() this
//                       task enters trampoline, which calls task.fn()
//
void ContextSwitcher::init_task(Task& task) {
    // 1. Get a valid baseline context (makecontext requires this)
    if (getcontext(&task.ctx) == -1) {
        throw std::runtime_error(
            std::string("getcontext failed for task ") + task.name);
    }

    // 2. Point the context at this task's private heap-allocated stack
    task.ctx.uc_stack.ss_sp   = task.stack.get();
    task.ctx.uc_stack.ss_size = Task::STACK_SIZE;

    // 3. When the task function returns naturally, fall through to idle_ctx_
    //    Without this, returning from a task causes undefined behaviour (crash).
    task.ctx.uc_link = &idle_ctx_;

    // 4. Set the entry point.
    //    makecontext modifies task.ctx so that the next swapcontext INTO it
    //    will begin execution at task_entry_trampoline().
    //    The '0' means we pass zero integer arguments to the trampoline.
    //    (Passing pointers via makecontext args is not portable on 64-bit.)
    makecontext(&task.ctx, task_entry_trampoline, 0);

    task.state = TaskState::READY;
}

// ─── switch_to ───────────────────────────────────────────────────────────────
// The actual context switch.
//
//   swapcontext(&from->ctx, &to->ctx) does two things atomically:
//     SAVE:    snapshot all callee-saved registers (rsp, rbp, r12–r15, rip…)
//              into from->ctx.uc_mcontext
//     RESTORE: load all registers from to->ctx.uc_mcontext
//              CPU instruction pointer jumps to wherever 'to' left off
//
// After this line executes, 'to' is running.
// This function "returns" only when something later switches BACK to 'from'.
//
void ContextSwitcher::switch_to(Task* from, Task* to) {
    // Update the thread-local so the trampoline knows who's entering
    current_task = to;

    // Update states
    if (from->state == TaskState::RUNNING) {
        from->state = TaskState::READY;
    }
    to->state = TaskState::RUNNING;
    to->switch_count++;

    // THE switch. Everything above this line runs in 'from'.
    // Everything after this line (when we eventually return) runs in 'from' again,
    // but potentially much later — after to has run and switched back.
    swapcontext(&from->ctx, &to->ctx);
}

// ─── enter ───────────────────────────────────────────────────────────────────
// First-time entry: switch from the bootstrap (main) context into the first task.
// We don't have a "from" Task, so we switch from idle_ctx_.
//
void ContextSwitcher::enter(Task* to) {
    current_task = to;
    to->state    = TaskState::RUNNING;
    to->switch_count++;

    // Save current execution state into idle_ctx_, jump into 'to'.
    // When all tasks finish and uc_link chains back to idle_ctx_,
    // swapcontext returns here and main() continues.
    swapcontext(&idle_ctx_, &to->ctx);
}

// ─── task_entry_trampoline ───────────────────────────────────────────────────
// Called by the CPU when a task is switched into for the very first time.
// Reads current_task (set just before the switch) and calls its fn().
//
// Why a trampoline?
//   makecontext needs a void(*)() — a plain C function pointer.
//   We can't pass a Task* as an argument portably on all 64-bit platforms
//   because makecontext's varargs are typed as int (32-bit).
//   The thread-local pointer sidesteps this cleanly.
//
// static so it has no implicit 'this' — required for makecontext.
//
void ContextSwitcher::task_entry_trampoline() {
    Task* task = current_task;  // read the thread-local set by switch_to/enter

    if (task && task->fn) {
        task->fn();             // ← run the actual user lambda/function
    }

    // Mark done. uc_link will now carry us back to idle_ctx_.
    if (task) {
        task->state = TaskState::DONE;
    }
    // Function returns here → CPU follows uc_link → back to idle_ctx_
}

}  // namespace helios