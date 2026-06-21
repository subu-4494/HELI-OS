#pragma once

#include <ucontext.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace helios {

// ─── Task states ────────────────────────────────────────────────────────────
// Mirrors a real RTOS task lifecycle.
// NEW      → task created, context not yet initialised
// READY    → on the ready queue, waiting for CPU
// RUNNING  → currently executing on CPU
// BLOCKED  → waiting for an event (sleep, mutex, I/O)
// DONE     → function returned, stack can be freed
enum class TaskState {
    NEW,
    READY,
    RUNNING,
    BLOCKED,
    DONE,
};

inline const char* state_name(TaskState s) {
    switch (s) {
        case TaskState::NEW:     return "NEW";
        case TaskState::READY:   return "READY";
        case TaskState::RUNNING: return "RUNNING";
        case TaskState::BLOCKED: return "BLOCKED";
        case TaskState::DONE:    return "DONE";
    }
    return "UNKNOWN";
}

// ─── Task Control Block ──────────────────────────────────────────────────────
// Everything the scheduler needs to know about one task.
// This is the userspace equivalent of the kernel's task_struct.
//
// Memory layout note:
//   ctx  lives here (in the TCB)
//   stack is heap-allocated separately and pointed to by ctx.uc_stack
//   fn   is the user's lambda/function, stored as std::function
//
struct Task {
    // ── Identity ──────────────────────────────────────────────────────────
    int         id;
    std::string name;

    // ── CPU state ─────────────────────────────────────────────────────────
    // ucontext_t holds the full register snapshot for this task:
    //   uc_mcontext  — general-purpose registers (rsp, rip, rbp, r12–r15…)
    //   uc_stack     — pointer + size of this task's private stack
    //   uc_link      — context to jump to when this task's function returns
    //
    // swapcontext(&A.ctx, &B.ctx) atomically:
    //   1. saves all callee-saved registers into A.ctx
    //   2. restores all registers from B.ctx
    //   3. CPU resumes executing B from wherever it left off
    ucontext_t ctx;

    // ── Private stack ─────────────────────────────────────────────────────
    // Each task needs its own stack so local variables, return addresses,
    // and saved registers don't collide between tasks.
    // 64 KB is generous for a demo; real RTOS tasks often use 512–4096 bytes.
    static constexpr std::size_t STACK_SIZE = 64 * 1024;  // 64 KB
    std::unique_ptr<char[]>      stack;

    // ── Scheduling metadata ───────────────────────────────────────────────
    // NOTE: declaration order must match constructor initialiser list order
    // (g++ -Wreorder enforces this)
    TaskState state        = TaskState::NEW;
    int       priority     = 0;    // higher = more urgent (used in later phases)
    uint64_t  created_at   = 0;    // nanoseconds (clock_gettime)
    uint64_t  started_at   = 0;
    uint64_t  finished_at  = 0;
    uint32_t  switch_count = 0;    // how many times this task has been switched in

    // ── User function ─────────────────────────────────────────────────────
    std::function<void()> fn;

    // ── Constructor ───────────────────────────────────────────────────────
    // Initialiser order matches declaration order above (required by -Wreorder)
    Task(int id, std::string name, int priority, std::function<void()> fn)
        : id(id)
        , name(std::move(name))
        , stack(std::make_unique<char[]>(STACK_SIZE))
        , priority(priority)
        , fn(std::move(fn))
    {}

    // Non-copyable — ucontext_t contains raw pointers into stack memory.
    // Moving would invalidate uc_stack.ss_sp. Always use shared_ptr<Task>.
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&)                 = delete;
    Task& operator=(Task&&)      = delete;
};

}  // namespace helios