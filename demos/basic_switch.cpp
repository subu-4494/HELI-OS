// ─────────────────────────────────────────────────────────────────────────────
// basic_switch.cpp  —  Helios Week 1 demo
//
// PURPOSE
//   Prove that ucontext_t context switching works correctly.
//   No scheduler, no signals, no preemption — just two tasks
//   manually handing control to each other via swapcontext().
//
// WHAT TO OBSERVE
//   Task A and Task B alternate perfectly.
//   Each task resumes exactly where it left off (local variables preserved).
//   After both finish, main() regains control cleanly.
//
// HOW TO BUILD
//   g++ -std=c++17 -O0 -g -I../core/include basic_switch.cpp ../core/src/context.cpp -o basic_switch
//
// HOW TO RUN
//   ./basic_switch
//
// EXPECTED OUTPUT
//   [Helios] Initialising two tasks...
//   [Task A] starting  (switch_count=1)
//   [Task B] starting  (switch_count=1)
//   [Task A] iteration 1  (local_counter=1)
//   [Task B] iteration 1  (local_counter=1)
//   [Task A] iteration 2  (local_counter=2)
//   [Task B] iteration 2  (local_counter=2)
//   ... (ITERATIONS times)
//   [Task A] done. Total switches: N
//   [Task B] done. Total switches: N
//   [Helios] Both tasks finished. Main context restored.
// ─────────────────────────────────────────────────────────────────────────────

#include "context.hpp"
#include "task.hpp"

#include <cstdio>
#include <ctime>
#include <memory>

using namespace helios;
using namespace std;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static uint64_t now_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

static void print_separator() {
    printf("─────────────────────────────────────────\n");
}

// ─── Global switcher + task handles ──────────────────────────────────────────
// These are global so each task's lambda can capture them by pointer.
// In later phases the Scheduler owns these; for now we keep it explicit.

static ContextSwitcher switcher;
static Task* task_a_ptr = nullptr;
static Task* task_b_ptr = nullptr;

// ─── How many times each task iterates before finishing ──────────────────────
static constexpr int ITERATIONS = 5;

// ─── Task A's body ───────────────────────────────────────────────────────────
//
// Key things to notice:
//   1. 'local_counter' is a stack variable inside Task A's private 64KB stack.
//      It survives across context switches because swapcontext saves/restores
//      rsp (stack pointer) and rbp (frame pointer).
//   2. Calling switcher.switch_to(task_a_ptr, task_b_ptr) suspends Task A
//      and resumes Task B. The line after switch_to() runs only when
//      something switches BACK to Task A.
//   3. This is cooperative switching — Task A explicitly yields.
//      Preemptive switching (SIGALRM) is added in Week 3.
//
static void task_a_body() {
    int local_counter = 0;  // lives on Task A's private stack → safe across switches

    printf("[Task A] starting  (switch_count=%u)\n",
                task_a_ptr->switch_count);

    // Immediately yield to B so B can print its "starting" line too
    switcher.switch_to(task_a_ptr, task_b_ptr);

    for (int i = 0; i < ITERATIONS; ++i) {
        ++local_counter;
        printf("[Task A] iteration %d  (local_counter=%d)\n",
                    i + 1, local_counter);
        fflush(stdout);

        if (i < ITERATIONS - 1) {
            // Yield to B — hand off control
            // This function returns when B switches back to us
            switcher.switch_to(task_a_ptr, task_b_ptr);
        }
    }

    std::printf("[Task A] done. Total switches: %u\n",
                task_a_ptr->switch_count);
}

// ─── Task B's body ───────────────────────────────────────────────────────────
//
// Symmetric to Task A.
// Notice that B's local_counter is completely independent of A's —
// they live on different stacks.
//
static void task_b_body() {
    int local_counter = 0;  // lives on Task B's private stack → independent of A

    printf("[Task B] starting  (switch_count=%u)\n",
                task_b_ptr->switch_count);

    for (int i = 0; i < ITERATIONS; ++i) {
        ++local_counter;
        printf("[Task B] iteration %d  (local_counter=%d)\n",
                    i + 1, local_counter);
        fflush(stdout);

        // Always switch back to A after each iteration
        // On the last iteration, A will finish and not switch back,
        // so B finishes too.
        switcher.switch_to(task_b_ptr, task_a_ptr);
    }

    printf("[Task B] done. Total switches: %u\n",
                task_b_ptr->switch_count);
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    print_separator();
    printf("[Helios] basic_switch demo — Week 1\n");
    printf("[Helios] Demonstrating ucontext_t context switching\n");
    print_separator();

    // ── Create Task A ────────────────────────────────────────────────────
    // id=1, name="TaskA", priority=0 (unused this week), fn=task_a_body
    auto task_a = std::make_shared<Task>(1, "TaskA", 0, task_a_body);
    auto task_b = std::make_shared<Task>(2, "TaskB", 0, task_b_body);

    // Set the global pointers so the lambdas can find each other
    task_a_ptr = task_a.get();
    task_b_ptr = task_b.get();

    // ── Initialise contexts ──────────────────────────────────────────────
    // This calls getcontext() + makecontext() for each task.
    // After init_task(), the task is READY but not yet running.
    printf("[Helios] Initialising Task A stack (%zu KB)...\n",
                Task::STACK_SIZE / 1024);
    switcher.init_task(*task_a);

    printf("[Helios] Initialising Task B stack (%zu KB)...\n",
                Task::STACK_SIZE / 1024);
    switcher.init_task(*task_b);

    printf("[Helios] Both tasks READY. Entering Task A...\n");
    print_separator();

    // ── Record wall time ─────────────────────────────────────────────────
    uint64_t t0 = now_ns();

    // ── Enter the first task ─────────────────────────────────────────────
    // enter() saves main()'s context into idle_ctx_ and jumps into Task A.
    // This call blocks (from main's perspective) until all tasks finish
    // and uc_link chains bring control back to idle_ctx_.
    task_a->state    = TaskState::RUNNING;
    task_a->started_at = t0;
    switcher.enter(task_a.get());

    // ── Back in main() ───────────────────────────────────────────────────
    // We're here because both tasks finished and the uc_link chain
    // returned to idle_ctx_ inside ContextSwitcher::enter().

    uint64_t t1 = now_ns();

    print_separator();
    printf("[Helios] Both tasks finished. Main context restored.\n\n");

    // ── Post-run diagnostics ─────────────────────────────────────────────
    printf("  Task A — state: %s, switches: %u\n",
                state_name(task_a->state), task_a->switch_count);
    printf("  Task B — state: %s, switches: %u\n",
                state_name(task_b->state), task_b->switch_count);
    printf("  Total wall time: %.3f ms\n",
                static_cast<double>(t1 - t0) / 1e6);
    printf("  Total context switches: %u\n",
                task_a->switch_count + task_b->switch_count);

    print_separator();
    printf("[Helios] What just happened (Week 1 recap):\n");
    printf("  Each task had its own private %zu KB stack.\n",
                Task::STACK_SIZE / 1024);
    printf("  swapcontext() saved+restored all callee-saved registers.\n");
    printf("  Local variables survived across every context switch.\n");
    printf("  main() regained control cleanly via uc_link → idle_ctx_.\n");
    print_separator();

    return 0;
}