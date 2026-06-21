// ─────────────────────────────────────────────────────────────────────────────
// stress_test.cpp  —  Helios Week 1 stress test
//
// PURPOSE
//   Verify that ContextSwitcher is correct under load — not just for 2 tasks
//   and 10 switches, but for 20 tasks and 1000 total switches.
//
// WHAT IT TESTS (5 checks)
//   [1] Run counts     — every task ran exactly ROUNDS times, none skipped
//   [2] Order          — tasks executed in strict ring order (0→1→2→...→19→0)
//   [3] Stack isolation — each task's local stack data survived all switches
//   [4] Switch counts  — TCB switch_count matches actual observed switches
//   [5] Memory         — run under Valgrind, expect 0 errors
//
// HOW TO BUILD
//   g++ -std=c++17 -O0 -g -I../core/include stress_test.cpp ../core/src/context.cpp -o stress_test
//
// HOW TO RUN
//   ./stress_test
//   valgrind --leak-check=full --track-origins=yes ./stress_test
//
// EXPECTED OUTPUT
//   [CHECK 1] Run counts ........... PASS  (20 tasks x 50 rounds = 1000 runs)
//   [CHECK 2] Execution order ...... PASS  (strict ring: 0→1→2→...→19→0)
//   [CHECK 3] Stack isolation ....... PASS (each task's canary survived)
//   [CHECK 4] Switch counts ........ PASS  (TCB counts match observations)
//   ─────────────────────────────────────────────────────────────
//   ALL CHECKS PASSED — context switcher is correct under load
// ─────────────────────────────────────────────────────────────────────────────

#include "context.hpp"
#include "task.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

using namespace helios;

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr int N      = 20;   // number of tasks
static constexpr int ROUNDS = 50;   // how many times each task runs

// ─── Shared test state ───────────────────────────────────────────────────────
// All globals are written by tasks and read by main() after all tasks finish.
// No synchronisation needed — tasks run one at a time (cooperative, no signals).

static ContextSwitcher switcher;

// Pointers to all tasks — tasks need these to call switch_to(self, next).
// Populated in main() before any task runs.
static std::vector<std::shared_ptr<Task>> tasks;

// [CHECK 1] How many times did each task actually execute its body?
static int run_counts[N] = {};

// [CHECK 2] Log of which task ran, in order. Expected: 0,1,2,...,19,0,1,...
// 20 tasks × 50 rounds = 1000 entries.
static int execution_log[N * ROUNDS] = {};
static int log_pos                   = 0;

// [CHECK 3] Stack canary — each task writes a unique pattern to a local array
// and checks it's intact after being switched out and back in.
// canary_failures[i] = number of times task i found its canary corrupted.
static int canary_failures[N] = {};

// [CHECK 4] How many times did main() observe each task being switched in?
// Compared against TCB switch_count after the run.
static int observed_switches[N] = {};

// ─── Utility ─────────────────────────────────────────────────────────────────

static uint64_t now_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

static void print_separator() {
    std::printf("─────────────────────────────────────────────────────────────\n");
}

// ─── Result printer ──────────────────────────────────────────────────────────

static void print_result(int check_num, const char* name, bool passed,
                         const char* detail = nullptr) {
    // Pad name to fixed width for alignment
    char padded[40];
    int len = static_cast<int>(strlen(name));
    memcpy(padded, name, static_cast<std::size_t>(len));
    for (int i = len; i < 38; i++) padded[i] = '.';
    padded[38] = '\0';

    if (passed) {
        std::printf("[CHECK %d] %s PASS", check_num, padded);
    } else {
        std::printf("[CHECK %d] %s FAIL  <<<", check_num, padded);
    }
    if (detail) std::printf("  %s", detail);
    std::printf("\n");
}

// ─── Task body ───────────────────────────────────────────────────────────────
// This is the function every task runs.
// Each task:
//   1. allocates a local stack canary (unique per-task pattern)
//   2. loops ROUNDS times:
//      a. checks its canary is intact (stack isolation)
//      b. records its run (execution log + run count)
//      c. switches to the next task in the ring
//      d. when it wakes back up, checks canary again
//   3. after ROUNDS iterations, yields one final time so the ring
//      can unwind cleanly back to task 0 → main()
//
static void task_body(int my_id) {
    int next_id = (my_id + 1) % N;

    // ── Stack canary ─────────────────────────────────────────────────────
    // Fill a local buffer with a pattern unique to this task:
    //   task 0  → 0x00, task 1 → 0x11, ..., task 15 → 0xFF, task 16 → 0x00...
    // If another task's context switch corrupts our stack, these bytes change.
    // The buffer sits on THIS task's private 64KB stack — completely separate
    // from all other tasks' stacks.
    unsigned char canary[256];
    unsigned char pattern = static_cast<unsigned char>(my_id * 13);  // unique per task
    memset(canary, pattern, sizeof(canary));

    for (int round = 0; round < ROUNDS; round++) {

        // ── Check canary before doing work ───────────────────────────────
        // On round 0 this is a baseline check.
        // On round 1+ this checks the canary survived being switched away.
        for (std::size_t b = 0; b < sizeof(canary); b++) {
            if (canary[b] != pattern) {
                canary_failures[my_id]++;
                break;  // one failure per round is enough to record
            }
        }

        // ── Record this execution ─────────────────────────────────────────
        run_counts[my_id]++;
        observed_switches[my_id]++;

        int pos = log_pos++;  // log_pos is written sequentially (one task at a time)
        if (pos < N * ROUNDS) {
            execution_log[pos] = my_id;
        }

        // ── Yield to next task ────────────────────────────────────────────
        // After this call returns, 'next' has run at least one round
        // and switched back to us. Our local variables (round, canary,
        // pattern, my_id, next_id) are all exactly as we left them.
        switcher.switch_to(tasks[my_id].get(), tasks[next_id].get());

        // ── Check canary after waking up ─────────────────────────────────
        // swapcontext must have restored rsp to our frame, so 'canary'
        // should still be at the same address with the same contents.
        for (std::size_t b = 0; b < sizeof(canary); b++) {
            if (canary[b] != pattern) {
                canary_failures[my_id]++;
                break;
            }
        }
    }

    // ── Final yield ───────────────────────────────────────────────────────
    // After ROUNDS iterations, this task is done but it must hand control
    // to the next task so the ring can finish. Without this, the last task
    // to run would jump to uc_link (idle_ctx_) while other tasks are still
    // mid-execution, and the ring would be broken.
    //
    // The final yield passes control to the next task. That task will
    // eventually exhaust its rounds, yield again, and so on until task 0
    // does its final yield — which lands back in a task that has already
    // finished (and immediately returns again via uc_link).
    //
    // This is the same "B stays READY" behaviour from basic_switch — once
    // every task has finished its ROUNDS, the yields cascade through the
    // ring and eventually unwind back to idle_ctx_ → main().
    switcher.switch_to(tasks[my_id].get(), tasks[next_id].get());

    // If we ever get switched back after finishing, just return.
    // task_entry_trampoline will set state = DONE.
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    print_separator();
    std::printf("[Helios] stress_test — Week 1\n");
    std::printf("[Helios] %d tasks x %d rounds = %d total context switches\n",
                N, ROUNDS, N * ROUNDS);
    print_separator();

    // ── Create and initialise all tasks ──────────────────────────────────
    tasks.reserve(N);
    for (int i = 0; i < N; i++) {
        // Each lambda captures only 'i' by value — a plain int, no dangling refs.
        // The lambda calls task_body(i) which holds all real logic.
        tasks.push_back(std::make_shared<Task>(
            i,
            "T" + std::to_string(i),
            0,
            [i]() { task_body(i); }
        ));
        switcher.init_task(*tasks[i]);
    }

    std::printf("[Helios] %d tasks initialised (%zu KB stack each, %zu KB total)\n",
                N,
                Task::STACK_SIZE / 1024,
                (N * Task::STACK_SIZE) / 1024);
    std::printf("[Helios] Entering task ring...\n\n");

    // ── Run ───────────────────────────────────────────────────────────────
    uint64_t t0 = now_ns();

    // Enter the ring at task 0. Control won't return to main() until the
    // entire ring has unwound (all tasks finished their ROUNDS + final yield).
    switcher.enter(tasks[0].get());

    uint64_t t1 = now_ns();

    // ─────────────────────────────────────────────────────────────────────
    // All tasks finished. Run verification checks.
    // ─────────────────────────────────────────────────────────────────────

    int total_failures = 0;

    // ── CHECK 1: Run counts ───────────────────────────────────────────────
    // Every task must have run exactly ROUNDS times.
    {
        bool passed   = true;
        int  total    = 0;
        char detail[80];

        for (int i = 0; i < N; i++) {
            total += run_counts[i];
            if (run_counts[i] != ROUNDS) {
                passed = false;
            }
        }

        if (passed) {
            snprintf(detail, sizeof(detail),
                     "(%d tasks x %d rounds = %d runs)", N, ROUNDS, total);
        } else {
            // Find first offender for the error message
            for (int i = 0; i < N; i++) {
                if (run_counts[i] != ROUNDS) {
                    snprintf(detail, sizeof(detail),
                             "(task %d ran %d times, expected %d)",
                             i, run_counts[i], ROUNDS);
                    break;
                }
            }
        }

        print_result(1, "Run counts", passed, detail);
        if (!passed) {
            total_failures++;
            // Print per-task breakdown so you can see which tasks failed
            for (int i = 0; i < N; i++) {
                if (run_counts[i] != ROUNDS) {
                    std::printf("         Task %2d: ran %d times (expected %d)\n",
                                i, run_counts[i], ROUNDS);
                }
            }
        }
    }

    // ── CHECK 2: Execution order ──────────────────────────────────────────
    // Tasks must have run in strict ring order: 0,1,2,...,19,0,1,...
    // Any deviation means switch_to() jumped to the wrong task.
    {
        bool passed = true;
        int  bad_at = -1;
        int  got    = -1;
        int  want   = -1;

        for (int pos = 0; pos < N * ROUNDS; pos++) {
            int expected = pos % N;
            if (execution_log[pos] != expected) {
                passed = false;
                bad_at = pos;
                got    = execution_log[pos];
                want   = expected;
                break;
            }
        }

        char detail[80];
        if (passed) {
            snprintf(detail, sizeof(detail),
                     "(strict ring: 0→1→...→%d→0 verified)", N - 1);
        } else {
            snprintf(detail, sizeof(detail),
                     "(pos %d: got task %d, expected task %d)", bad_at, got, want);
        }
        print_result(2, "Execution order", passed, detail);
        if (!passed) total_failures++;
    }

    // ── CHECK 3: Stack isolation ──────────────────────────────────────────
    // No task should have found its canary corrupted.
    // A non-zero canary_failures[i] means task i's stack was overwritten
    // by another task's context — the stacks are not truly isolated.
    {
        bool passed       = true;
        int  total_bad    = 0;
        int  worst_task   = -1;
        int  worst_count  = 0;

        for (int i = 0; i < N; i++) {
            if (canary_failures[i] > 0) {
                passed = false;
                total_bad += canary_failures[i];
                if (canary_failures[i] > worst_count) {
                    worst_count = canary_failures[i];
                    worst_task  = i;
                }
            }
        }

        char detail[80];
        if (passed) {
            snprintf(detail, sizeof(detail),
                     "(each task's 256-byte canary intact across all switches)");
        } else {
            snprintf(detail, sizeof(detail),
                     "(%d total corruptions, worst: task %d with %d)",
                     total_bad, worst_task, worst_count);
        }
        print_result(3, "Stack isolation", passed, detail);
        if (!passed) {
            total_failures++;
            for (int i = 0; i < N; i++) {
                if (canary_failures[i] > 0) {
                    std::printf("         Task %2d: canary corrupted %d times\n",
                                i, canary_failures[i]);
                }
            }
        }
    }

    // ── CHECK 4: TCB switch counts ────────────────────────────────────────
    // Each task's TCB switch_count (incremented in switch_to/enter) must
    // match the observed_switches[] we tracked manually inside task_body.
    // A mismatch means switch_to() is miscounting or double-counting.
    //
    // Note: task 0 gets +1 for the initial enter() call, so its
    // switch_count = observed_switches[0] + 1 is expected.
    {
        bool passed = true;
        int  bad_id = -1;

        for (int i = 0; i < N; i++) {
            int tcb_count = static_cast<int>(tasks[i]->switch_count);
            int expected = observed_switches[i] + 1 + (i == 0 ? 1 : 0); // +1 for final yield
            if (tcb_count != expected) {
                passed = false;
                bad_id = i;
                break;
            }
        }

        char detail[80];
        if (passed) {
            snprintf(detail, sizeof(detail),
                     "(TCB counts match observed switches for all %d tasks)", N);
        } else {
            int tcb = static_cast<int>(tasks[bad_id]->switch_count);
            int exp = observed_switches[bad_id] + 1 + (bad_id == 0 ? 1 : 0);
            snprintf(detail, sizeof(detail),
                     "(task %d: TCB says %d, expected %d)", bad_id, tcb, exp);
        }
        print_result(4, "Switch counts", passed, detail);
        if (!passed) total_failures++;
    }

    // ── Summary ───────────────────────────────────────────────────────────
    std::printf("\n");
    print_separator();

    double elapsed_ms = static_cast<double>(t1 - t0) / 1e6;
    int    total_switches = 0;
    for (int i = 0; i < N; i++) {
        total_switches += static_cast<int>(tasks[i]->switch_count);
    }

    if (total_failures == 0) {
        std::printf("ALL CHECKS PASSED — context switcher is correct under load\n\n");
        std::printf("  Tasks:            %d\n", N);
        std::printf("  Rounds each:      %d\n", ROUNDS);
        std::printf("  Total switches:   %d\n", total_switches);
        std::printf("  Wall time:        %.3f ms\n", elapsed_ms);
        std::printf("  Time per switch:  %.2f µs\n",
                    (elapsed_ms * 1000.0) / total_switches);
        std::printf("  Stack memory:     %zu KB (%d tasks x %zu KB)\n",
                    (N * Task::STACK_SIZE) / 1024, N, Task::STACK_SIZE / 1024);
    } else {
        std::printf("%d CHECK(S) FAILED — see details above\n", total_failures);
        std::printf("Run under Valgrind for memory error details:\n");
        std::printf("  valgrind --track-origins=yes ./stress_test\n");
    }

    print_separator();

    return total_failures == 0 ? 0 : 1;
}