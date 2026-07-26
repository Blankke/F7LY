/*
 * schedbench_improved.c
 *
 * Portable SMP scheduler benchmark for StarryOS and Linux.
 *
 * Build:
 *   cc -O2 -pthread schedbench_improved.c -lm -o schedbench
 *
 * Examples:
 *   ./schedbench wake --pattern fanout --workers 8 --cpus 4 \
 *       --warmup 256 --rounds 4096
 *
 *   ./schedbench wake --pattern broadcast --workers 8 --cpus 4 \
 *       --warmup 256 --rounds 4096
 *
 *   ./schedbench migrate --pattern independent --workers 4 --cpus 4 \
 *       --warmup 128 --rounds 4096 --working-set-kib 256
 *
 *   ./schedbench migrate --pattern wave --workers 4 --cpus 4 \
 *       --warmup 64 --rounds 1024 --working-set-kib 256
 *
 *   ./schedbench suite --workers 8 --cpus 4 --warmup 256 --rounds 4096 \
 *       --migration-pattern independent --working-set-kib 256
 *
 * Design:
 *   - No printf/malloc in measured loops.
 *   - Every sample is stored in preallocated memory.
 *   - Wake and migration workloads have deterministic setup.
 *   - Migration confirms the task actually executes on the requested CPU.
 *   - Results are emitted in raw nanoseconds for machine parsing and in
 *     microseconds for human reading.
 *   - A phase watchdog makes hangs distinguishable from normal failures.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_BENCH_CPUS = 64,
    DEFAULT_WORKERS = 8,
    DEFAULT_CPUS = 4,
    DEFAULT_ROUNDS = 4096,
    DEFAULT_WARMUP_ROUNDS = 256,
    DEFAULT_WORKING_SET_KIB = 256,
    DEFAULT_MIGRATION_TIMEOUT_US = 500000,
    DEFAULT_PHASE_TIMEOUT_SEC = 180,
};

enum bench_kind {
    BENCH_WAKE,
    BENCH_MIGRATE,
    BENCH_SUITE,
};

enum wake_pattern {
    WAKE_FANOUT,
    WAKE_BROADCAST,
};

enum migration_pattern {
    MIGRATE_INDEPENDENT,
    MIGRATE_WAVE,
    MIGRATE_BOTH,
};

enum watchdog_phase {
    WATCHDOG_NONE = 0,
    WATCHDOG_WAKE_FANOUT,
    WATCHDOG_WAKE_BROADCAST,
    WATCHDOG_MIGRATE_INDEPENDENT,
    WATCHDOG_MIGRATE_WAVE,
};

struct bench_config {
    enum bench_kind kind;
    enum wake_pattern wake_pattern;
    enum migration_pattern migration_pattern;
    unsigned workers;
    unsigned cpus;
    unsigned rounds;
    unsigned warmup_rounds;
    unsigned working_set_kib;
    unsigned migration_timeout_us;
    unsigned phase_timeout_sec;
    unsigned migration_trace_every;
};

struct bench_barrier {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_cond_t start;
    pthread_cond_t completed;
    unsigned ready_count;
    unsigned completed_count;
    bool start_released;
};

struct generation_barrier {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned arrived;
    unsigned generation;
};

struct wake_sample {
    /* Signal/broadcast publication to this worker resuming. */
    uint64_t latency_ns;
    /* Start of the whole round to this worker resuming. */
    uint64_t round_latency_ns;
    int resume_cpu;
};

struct migration_sample {
    uint64_t setaffinity_latency_ns;
    uint64_t completion_latency_ns;
    int source_cpu;
    int target_cpu;
    int observed_cpu;
    int affinity_errno;
    bool timed_out;
};

struct wake_shared {
    struct bench_config config;
    struct bench_barrier barrier;

    pthread_mutex_t broadcast_mutex;
    pthread_cond_t broadcast_cond;
    uint64_t broadcast_sequence;
    uint64_t broadcast_publish_ns;
};

struct wake_worker {
    struct wake_shared *shared;
    unsigned worker_id;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint64_t sequence;
    uint64_t publish_ns;
    uint64_t round_start_ns;
    bool stop;

    int setup_error;
    uint64_t checksum;
    struct wake_sample *samples;
};

struct migration_shared {
    struct bench_config config;
    struct bench_barrier barrier;
    struct generation_barrier generation_barrier;
    uint64_t measurement_start_ns;
};

struct migration_worker {
    struct migration_shared *shared;
    unsigned worker_id;

    int setup_error;
    unsigned affinity_errors;
    struct migration_sample *samples;

    uint8_t *working_set;
    size_t working_set_bytes;
    uint64_t checksum;
    unsigned data_errors;
};

static volatile sig_atomic_t g_watchdog_phase = WATCHDOG_NONE;

static void fatal_errno(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

static void check_pthread(int result, const char *operation)
{
    if (result != 0) {
        errno = result;
        fatal_errno(operation);
    }
}

static void *xcalloc(size_t count, size_t size, const char *what)
{
    if (size != 0 && count > SIZE_MAX / size) {
        fprintf(stderr, "allocation overflow for %s\n", what);
        exit(EXIT_FAILURE);
    }

    void *memory = calloc(count, size);
    if (memory == NULL) {
        fprintf(stderr, "allocation failed for %s\n", what);
        exit(EXIT_FAILURE);
    }
    return memory;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        fatal_errno("clock_gettime");
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static double ns_to_us(uint64_t nanoseconds)
{
    return (double)nanoseconds / 1000.0;
}

static void benchmark_timeout_handler(int signal_number)
{
    (void)signal_number;

    static const char generic[] =
        "SCHEDBENCH_TIMEOUT phase=unknown\n";
    static const char fanout[] =
        "SCHEDBENCH_TIMEOUT phase=wake-fanout\n";
    static const char broadcast[] =
        "SCHEDBENCH_TIMEOUT phase=wake-broadcast\n";
    static const char migrate_independent[] =
        "SCHEDBENCH_TIMEOUT phase=migrate-independent\n";
    static const char migrate_wave[] =
        "SCHEDBENCH_TIMEOUT phase=migrate-wave\n";

    const char *message = generic;
    size_t length = sizeof(generic) - 1;

    switch (g_watchdog_phase) {
    case WATCHDOG_WAKE_FANOUT:
        message = fanout;
        length = sizeof(fanout) - 1;
        break;
    case WATCHDOG_WAKE_BROADCAST:
        message = broadcast;
        length = sizeof(broadcast) - 1;
        break;
    case WATCHDOG_MIGRATE_INDEPENDENT:
        message = migrate_independent;
        length = sizeof(migrate_independent) - 1;
        break;
    case WATCHDOG_MIGRATE_WAVE:
        message = migrate_wave;
        length = sizeof(migrate_wave) - 1;
        break;
    default:
        break;
    }

    const ssize_t write_result = write(STDOUT_FILENO, message, length);
    (void)write_result;
    _exit(124);
}

static void arm_watchdog(enum watchdog_phase phase, unsigned timeout_sec)
{
    g_watchdog_phase = (sig_atomic_t)phase;
    alarm(timeout_sec);
}

static void disarm_watchdog(void)
{
    alarm(0);
    g_watchdog_phase = WATCHDOG_NONE;
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t left_value = *(const uint64_t *)left;
    const uint64_t right_value = *(const uint64_t *)right;

    return (left_value > right_value) - (left_value < right_value);
}

/* Nearest-rank percentile: ceil(p * count) - 1. */
static uint64_t percentile_per_mille(const uint64_t *values, size_t count, unsigned per_mille)
{
    if (count == 0) {
        return 0;
    }

    size_t rank = (count * per_mille + 999) / 1000;
    if (rank == 0) {
        rank = 1;
    }
    if (rank > count) {
        rank = count;
    }
    return values[rank - 1];
}

static int pin_current_to_cpu(unsigned cpu)
{
    cpu_set_t mask;

    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    return sched_setaffinity(0, sizeof(mask), &mask);
}

static int allow_all_benchmark_cpus(unsigned cpu_count)
{
    cpu_set_t mask;

    CPU_ZERO(&mask);
    for (unsigned cpu = 0; cpu < cpu_count; ++cpu) {
        CPU_SET(cpu, &mask);
    }
    return sched_setaffinity(0, sizeof(mask), &mask);
}

/*
 * sched_setaffinity() returning successfully is not itself the benchmark's
 * definition of completed migration. Setup waits until sched_getcpu() confirms
 * that the thread is actually executing on the requested CPU.
 */
static int pin_current_and_wait(unsigned cpu, unsigned timeout_us)
{
    if (pin_current_to_cpu(cpu) != 0) {
        return errno;
    }

    const uint64_t deadline =
        monotonic_ns() + (uint64_t)timeout_us * UINT64_C(1000);

    while (sched_getcpu() != (int)cpu) {
        if (monotonic_ns() >= deadline) {
            return ETIMEDOUT;
        }
        sched_yield();
    }
    return 0;
}

static void barrier_init(struct bench_barrier *barrier)
{
    check_pthread(pthread_mutex_init(&barrier->mutex, NULL), "pthread_mutex_init barrier");
    check_pthread(pthread_cond_init(&barrier->ready, NULL), "pthread_cond_init ready");
    check_pthread(pthread_cond_init(&barrier->start, NULL), "pthread_cond_init start");
    check_pthread(pthread_cond_init(&barrier->completed, NULL), "pthread_cond_init completed");

    barrier->ready_count = 0;
    barrier->completed_count = 0;
    barrier->start_released = false;
}

static void barrier_destroy(struct bench_barrier *barrier)
{
    check_pthread(pthread_cond_destroy(&barrier->completed), "pthread_cond_destroy completed");
    check_pthread(pthread_cond_destroy(&barrier->start), "pthread_cond_destroy start");
    check_pthread(pthread_cond_destroy(&barrier->ready), "pthread_cond_destroy ready");
    check_pthread(pthread_mutex_destroy(&barrier->mutex), "pthread_mutex_destroy barrier");
}

static void worker_mark_ready(struct bench_barrier *barrier)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock ready");
    ++barrier->ready_count;
    check_pthread(pthread_cond_signal(&barrier->ready), "pthread_cond_signal ready");
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "pthread_mutex_unlock ready");
}

static void wait_for_workers_ready(struct bench_barrier *barrier, unsigned workers)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock wait ready");
    while (barrier->ready_count < workers) {
        check_pthread(pthread_cond_wait(&barrier->ready, &barrier->mutex),
                      "pthread_cond_wait ready");
    }
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "pthread_mutex_unlock wait ready");
}

static void wait_for_benchmark_start(struct bench_barrier *barrier)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock wait start");
    while (!barrier->start_released) {
        check_pthread(pthread_cond_wait(&barrier->start, &barrier->mutex),
                      "pthread_cond_wait start");
    }
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "pthread_mutex_unlock wait start");
}

static void release_benchmark_start(struct bench_barrier *barrier)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock release start");
    barrier->start_released = true;
    check_pthread(pthread_cond_broadcast(&barrier->start), "pthread_cond_broadcast start");
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "pthread_mutex_unlock release start");
}

static void begin_round(struct bench_barrier *barrier)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock begin round");
    barrier->completed_count = 0;
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "pthread_mutex_unlock begin round");
}

static void worker_mark_completed(struct bench_barrier *barrier)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock complete");
    ++barrier->completed_count;
    check_pthread(pthread_cond_signal(&barrier->completed), "pthread_cond_signal complete");
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "pthread_mutex_unlock complete");
}

static void wait_for_round_completion(struct bench_barrier *barrier, unsigned workers)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock wait complete");
    while (barrier->completed_count < workers) {
        check_pthread(pthread_cond_wait(&barrier->completed, &barrier->mutex),
                      "pthread_cond_wait complete");
    }
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "pthread_mutex_unlock wait complete");
}

static void generation_barrier_init(struct generation_barrier *barrier)
{
    check_pthread(pthread_mutex_init(&barrier->mutex, NULL),
                  "pthread_mutex_init generation");
    check_pthread(pthread_cond_init(&barrier->cond, NULL),
                  "pthread_cond_init generation");
    barrier->arrived = 0;
    barrier->generation = 0;
}

static void generation_barrier_destroy(struct generation_barrier *barrier)
{
    check_pthread(pthread_cond_destroy(&barrier->cond),
                  "pthread_cond_destroy generation");
    check_pthread(pthread_mutex_destroy(&barrier->mutex),
                  "pthread_mutex_destroy generation");
}

/*
 * If release_timestamp_ns is non-NULL, the final arriving worker records the
 * common start of the measured section immediately before releasing the group.
 */
static void generation_barrier_wait(struct generation_barrier *barrier,
                                    unsigned participants,
                                    uint64_t *release_timestamp_ns)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex),
                  "pthread_mutex_lock generation");

    const unsigned generation = barrier->generation;
    ++barrier->arrived;

    if (barrier->arrived == participants) {
        barrier->arrived = 0;
        ++barrier->generation;
        if (release_timestamp_ns != NULL) {
            *release_timestamp_ns = monotonic_ns();
        }
        check_pthread(pthread_cond_broadcast(&barrier->cond),
                      "pthread_cond_broadcast generation");
    } else {
        while (generation == barrier->generation) {
            check_pthread(pthread_cond_wait(&barrier->cond, &barrier->mutex),
                          "pthread_cond_wait generation");
        }
    }

    check_pthread(pthread_mutex_unlock(&barrier->mutex),
                  "pthread_mutex_unlock generation");
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s wake --pattern fanout|broadcast [options]\n"
            "  %s migrate --pattern independent|wave [options]\n"
            "  %s suite [--migration-pattern independent|wave|both] [options]\n"
            "\n"
            "Common options:\n"
            "  --workers N\n"
            "  --cpus N\n"
            "  --rounds N\n"
            "  --warmup N\n"
            "  --timeout-sec N\n"
            "\n"
            "Migration options:\n"
            "  --working-set-kib N\n"
            "  --migration-timeout-us N\n"
            "  --trace N (diagnostic: emit every Nth migration epoch; 0 disables)\n",
            program, program, program);
}

static unsigned parse_positive_unsigned(const char *name, const char *text)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);

    if (*text == '\0' || *end != '\0' || value == 0 || value > UINT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (unsigned)value;
}

static unsigned parse_nonnegative_unsigned(const char *name, const char *text)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);

    if (*text == '\0' || *end != '\0' || value > UINT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (unsigned)value;
}

static enum wake_pattern parse_wake_pattern(const char *text)
{
    if (strcmp(text, "fanout") == 0) {
        return WAKE_FANOUT;
    }
    if (strcmp(text, "broadcast") == 0) {
        return WAKE_BROADCAST;
    }

    fprintf(stderr, "invalid wake pattern: %s\n", text);
    exit(EXIT_FAILURE);
}

static enum migration_pattern parse_migration_pattern(const char *text, bool allow_both)
{
    if (strcmp(text, "independent") == 0) {
        return MIGRATE_INDEPENDENT;
    }
    if (strcmp(text, "wave") == 0) {
        return MIGRATE_WAVE;
    }
    if (allow_both && strcmp(text, "both") == 0) {
        return MIGRATE_BOTH;
    }

    fprintf(stderr, "invalid migration pattern: %s\n", text);
    exit(EXIT_FAILURE);
}

static struct bench_config parse_config(int argc, char **argv)
{
    struct bench_config config = {
        .wake_pattern = WAKE_FANOUT,
        .migration_pattern = MIGRATE_INDEPENDENT,
        .workers = DEFAULT_WORKERS,
        .cpus = DEFAULT_CPUS,
        .rounds = DEFAULT_ROUNDS,
        .warmup_rounds = DEFAULT_WARMUP_ROUNDS,
        .working_set_kib = DEFAULT_WORKING_SET_KIB,
        .migration_timeout_us = DEFAULT_MIGRATION_TIMEOUT_US,
        .phase_timeout_sec = DEFAULT_PHASE_TIMEOUT_SEC,
        .migration_trace_every = 0,
    };

    if (argc < 2) {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (strcmp(argv[1], "wake") == 0) {
        config.kind = BENCH_WAKE;
    } else if (strcmp(argv[1], "migrate") == 0) {
        config.kind = BENCH_MIGRATE;
    } else if (strcmp(argv[1], "suite") == 0) {
        config.kind = BENCH_SUITE;
    } else {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    for (int index = 2; index < argc; index += 2) {
        if (index + 1 >= argc) {
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }

        const char *option = argv[index];
        const char *value = argv[index + 1];

        if (strcmp(option, "--pattern") == 0) {
            if (config.kind == BENCH_WAKE) {
                config.wake_pattern = parse_wake_pattern(value);
            } else if (config.kind == BENCH_MIGRATE) {
                config.migration_pattern = parse_migration_pattern(value, false);
            } else {
                fprintf(stderr, "--pattern is ambiguous for suite; use --migration-pattern\n");
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(option, "--migration-pattern") == 0
                   && config.kind == BENCH_SUITE) {
            config.migration_pattern = parse_migration_pattern(value, true);
        } else if (strcmp(option, "--workers") == 0) {
            config.workers = parse_positive_unsigned("worker count", value);
        } else if (strcmp(option, "--cpus") == 0) {
            config.cpus = parse_positive_unsigned("CPU count", value);
        } else if (strcmp(option, "--rounds") == 0) {
            config.rounds = parse_positive_unsigned("round count", value);
        } else if (strcmp(option, "--warmup") == 0) {
            config.warmup_rounds = parse_nonnegative_unsigned("warmup round count", value);
        } else if (strcmp(option, "--working-set-kib") == 0
                   && config.kind != BENCH_WAKE) {
            config.working_set_kib = parse_nonnegative_unsigned("working set KiB", value);
        } else if (strcmp(option, "--migration-timeout-us") == 0
                   && config.kind != BENCH_WAKE) {
            config.migration_timeout_us =
                parse_positive_unsigned("migration timeout in microseconds", value);
        } else if (strcmp(option, "--timeout-sec") == 0) {
            config.phase_timeout_sec =
                parse_positive_unsigned("phase timeout in seconds", value);
        } else if (strcmp(option, "--trace") == 0
                   && config.kind != BENCH_WAKE) {
            config.migration_trace_every =
                parse_nonnegative_unsigned("migration trace interval", value);
        } else {
            fprintf(stderr, "invalid option for selected mode: %s %s\n", option, value);
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (config.cpus > MAX_BENCH_CPUS || config.cpus > CPU_SETSIZE) {
        fprintf(stderr, "CPU count must not exceed %d\n",
                MAX_BENCH_CPUS < CPU_SETSIZE ? MAX_BENCH_CPUS : CPU_SETSIZE);
        exit(EXIT_FAILURE);
    }

    if ((config.kind == BENCH_MIGRATE || config.kind == BENCH_SUITE)
        && config.cpus < 2) {
        fprintf(stderr, "migration benchmark requires at least 2 CPUs\n");
        exit(EXIT_FAILURE);
    }

    return config;
}

static void run_busy_work(uint64_t *checksum, unsigned worker_id)
{
    *checksum = (*checksum << 7)
              ^ (*checksum >> 3)
              ^ UINT64_C(0x9e3779b97f4a7c15)
              ^ worker_id;
}

static void *wake_fanout_worker(void *opaque)
{
    struct wake_worker *worker = opaque;
    const struct bench_config *config = &worker->shared->config;
    uint64_t observed_sequence = 0;
    uint64_t checksum = worker->worker_id + 1;

    worker->setup_error = allow_all_benchmark_cpus(config->cpus) == 0 ? 0 : errno;
    worker_mark_ready(&worker->shared->barrier);

    for (;;) {
        uint64_t publish_ns;
        uint64_t round_start_ns;

        check_pthread(pthread_mutex_lock(&worker->mutex),
                      "pthread_mutex_lock fanout worker");
        while (!worker->stop && worker->sequence == observed_sequence) {
            check_pthread(pthread_cond_wait(&worker->cond, &worker->mutex),
                          "pthread_cond_wait fanout worker");
        }

        if (worker->stop) {
            check_pthread(pthread_mutex_unlock(&worker->mutex),
                          "pthread_mutex_unlock fanout stop");
            worker->checksum = checksum;
            return NULL;
        }

        observed_sequence = worker->sequence;
        publish_ns = worker->publish_ns;
        round_start_ns = worker->round_start_ns;

        check_pthread(pthread_mutex_unlock(&worker->mutex),
                      "pthread_mutex_unlock fanout worker");

        const uint64_t resume_ns = monotonic_ns();
        const unsigned iteration = (unsigned)(observed_sequence - 1);

        if (iteration >= config->warmup_rounds) {
            const unsigned sample_index = iteration - config->warmup_rounds;
            worker->samples[sample_index] = (struct wake_sample){
                .latency_ns = resume_ns - publish_ns,
                .round_latency_ns = resume_ns - round_start_ns,
                .resume_cpu = sched_getcpu(),
            };
        }

        run_busy_work(&checksum, worker->worker_id);
        worker_mark_completed(&worker->shared->barrier);
    }
}

static void *wake_broadcast_worker(void *opaque)
{
    struct wake_worker *worker = opaque;
    const struct bench_config *config = &worker->shared->config;
    const unsigned total_iterations = config->warmup_rounds + config->rounds;
    uint64_t observed_sequence = 0;
    uint64_t checksum = worker->worker_id + 1;

    worker->setup_error = allow_all_benchmark_cpus(config->cpus) == 0 ? 0 : errno;
    worker_mark_ready(&worker->shared->barrier);

    for (unsigned iteration = 0; iteration < total_iterations; ++iteration) {
        uint64_t publish_ns;

        check_pthread(pthread_mutex_lock(&worker->shared->broadcast_mutex),
                      "pthread_mutex_lock broadcast worker");
        while (worker->shared->broadcast_sequence == observed_sequence) {
            check_pthread(
                pthread_cond_wait(&worker->shared->broadcast_cond,
                                  &worker->shared->broadcast_mutex),
                "pthread_cond_wait broadcast worker");
        }

        observed_sequence = worker->shared->broadcast_sequence;
        publish_ns = worker->shared->broadcast_publish_ns;

        check_pthread(pthread_mutex_unlock(&worker->shared->broadcast_mutex),
                      "pthread_mutex_unlock broadcast worker");

        const uint64_t resume_ns = monotonic_ns();

        if (iteration >= config->warmup_rounds) {
            const unsigned sample_index = iteration - config->warmup_rounds;
            worker->samples[sample_index] = (struct wake_sample){
                .latency_ns = resume_ns - publish_ns,
                .round_latency_ns = resume_ns - publish_ns,
                .resume_cpu = sched_getcpu(),
            };
        }

        run_busy_work(&checksum, worker->worker_id);
        worker_mark_completed(&worker->shared->barrier);
    }

    worker->checksum = checksum;
    return NULL;
}

static int print_wake_result(const struct wake_shared *shared,
                             const struct wake_worker *workers,
                             uint64_t wall_ns)
{
    const struct bench_config *config = &shared->config;
    const size_t sample_count = (size_t)config->workers * config->rounds;

    uint64_t *latencies =
        xcalloc(sample_count, sizeof(*latencies), "wake latencies");
    uint64_t *round_first =
        xcalloc(config->rounds, sizeof(*round_first), "wake round first");
    uint64_t *round_last =
        xcalloc(config->rounds, sizeof(*round_last), "wake round last");

    uint64_t cpu_hits[MAX_BENCH_CPUS] = { 0 };
    uint64_t checksum = 0;
    unsigned errors = 0;
    size_t cursor = 0;

    for (unsigned worker_id = 0; worker_id < config->workers; ++worker_id) {
        errors += workers[worker_id].setup_error != 0;
        checksum ^= workers[worker_id].checksum;

        for (unsigned round = 0; round < config->rounds; ++round) {
            const struct wake_sample *sample = &workers[worker_id].samples[round];
            latencies[cursor++] = sample->latency_ns;

            if (sample->resume_cpu < 0
                || (unsigned)sample->resume_cpu >= config->cpus) {
                ++errors;
            } else {
                ++cpu_hits[sample->resume_cpu];
            }
        }

        printf("SCHEDBENCH_WORKER kind=wake pattern=%s worker=%u "
               "first_resume_cpu=%d samples=%u setup_error=%d checksum=%" PRIu64 "\n",
               config->wake_pattern == WAKE_FANOUT ? "fanout" : "broadcast",
               worker_id,
               workers[worker_id].samples[0].resume_cpu,
               config->rounds,
               workers[worker_id].setup_error,
               workers[worker_id].checksum);
    }

    for (unsigned round = 0; round < config->rounds; ++round) {
        uint64_t first = UINT64_MAX;
        uint64_t last = 0;

        for (unsigned worker_id = 0; worker_id < config->workers; ++worker_id) {
            const uint64_t latency =
                workers[worker_id].samples[round].round_latency_ns;
            if (latency < first) {
                first = latency;
            }
            if (latency > last) {
                last = latency;
            }
        }

        round_first[round] = first;
        round_last[round] = last;
    }

    qsort(latencies, sample_count, sizeof(*latencies), compare_u64);
    qsort(round_first, config->rounds, sizeof(*round_first), compare_u64);
    qsort(round_last, config->rounds, sizeof(*round_last), compare_u64);

    unsigned cpus_seen = 0;
    double sum_squared_distance = 0.0;
    const double mean = (double)sample_count / config->cpus;

    for (unsigned cpu = 0; cpu < config->cpus; ++cpu) {
        const double distance = (double)cpu_hits[cpu] - mean;
        cpus_seen += cpu_hits[cpu] != 0;
        sum_squared_distance += distance * distance;

        printf("SCHEDBENCH_CPU kind=wake pattern=%s cpu=%u samples=%" PRIu64 "\n",
               config->wake_pattern == WAKE_FANOUT ? "fanout" : "broadcast",
               cpu,
               cpu_hits[cpu]);
    }

    const double imbalance_cv =
        mean == 0.0 ? 0.0 : sqrt(sum_squared_distance / config->cpus) / mean;
    const double wakeups_per_s =
        wall_ns == 0 ? 0.0 : (double)sample_count * 1e9 / wall_ns;
    const double rounds_per_s =
        wall_ns == 0 ? 0.0 : (double)config->rounds * 1e9 / wall_ns;
    const char *pattern =
        config->wake_pattern == WAKE_FANOUT ? "fanout" : "broadcast";

    const uint64_t p50 = percentile_per_mille(latencies, sample_count, 500);
    const uint64_t p90 = percentile_per_mille(latencies, sample_count, 900);
    const uint64_t p99 = percentile_per_mille(latencies, sample_count, 990);
    const uint64_t p999 = percentile_per_mille(latencies, sample_count, 999);
    const uint64_t maximum = latencies[sample_count - 1];

    const uint64_t first_p50 =
        percentile_per_mille(round_first, config->rounds, 500);
    const uint64_t first_p99 =
        percentile_per_mille(round_first, config->rounds, 990);
    const uint64_t first_max = round_first[config->rounds - 1];

    const uint64_t last_p50 =
        percentile_per_mille(round_last, config->rounds, 500);
    const uint64_t last_p99 =
        percentile_per_mille(round_last, config->rounds, 990);
    const uint64_t last_max = round_last[config->rounds - 1];

    printf("SCHEDBENCH_RESULT kind=wake pattern=%s workers=%u cpus=%u "
           "warmup=%u rounds=%u samples=%zu wall_ns=%" PRIu64
           " wakeups_per_s=%.2f rounds_per_s=%.2f "
           "p50_ns=%" PRIu64 " p90_ns=%" PRIu64
           " p99_ns=%" PRIu64 " p999_ns=%" PRIu64
           " max_ns=%" PRIu64 " cpus_seen=%u spread_efficiency=%.4f "
           "imbalance_cv=%.4f errors=%u checksum=%" PRIu64 "\n",
           pattern,
           config->workers,
           config->cpus,
           config->warmup_rounds,
           config->rounds,
           sample_count,
           wall_ns,
           wakeups_per_s,
           rounds_per_s,
           p50,
           p90,
           p99,
           p999,
           maximum,
           cpus_seen,
           (double)cpus_seen / config->cpus,
           imbalance_cv,
           errors,
           checksum);

    printf("SCHEDBENCH_GROUP kind=wake pattern=%s "
           "first_p50_ns=%" PRIu64 " first_p99_ns=%" PRIu64
           " first_max_ns=%" PRIu64 " last_p50_ns=%" PRIu64
           " last_p99_ns=%" PRIu64 " last_max_ns=%" PRIu64 "\n",
           pattern,
           first_p50,
           first_p99,
           first_max,
           last_p50,
           last_p99,
           last_max);

    printf("SCHEDBENCH_HUMAN kind=wake pattern=%s "
           "p50_us=%.3f p90_us=%.3f p99_us=%.3f p999_us=%.3f max_us=%.3f "
           "group_first_p50_us=%.3f group_first_p99_us=%.3f "
           "group_last_p50_us=%.3f group_last_p99_us=%.3f\n",
           pattern,
           ns_to_us(p50),
           ns_to_us(p90),
           ns_to_us(p99),
           ns_to_us(p999),
           ns_to_us(maximum),
           ns_to_us(first_p50),
           ns_to_us(first_p99),
           ns_to_us(last_p50),
           ns_to_us(last_p99));

    free(round_last);
    free(round_first);
    free(latencies);

    return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_wake_benchmark(struct bench_config config)
{
    struct wake_shared shared = { .config = config };
    struct wake_worker *workers =
        xcalloc(config.workers, sizeof(*workers), "wake workers");
    pthread_t *threads =
        xcalloc(config.workers, sizeof(*threads), "wake threads");

    const int waker_pin_error =
        pin_current_and_wait(0, config.migration_timeout_us);
    if (waker_pin_error != 0) {
        errno = waker_pin_error;
        fatal_errno("pin benchmark waker to CPU 0");
    }

    barrier_init(&shared.barrier);
    check_pthread(pthread_mutex_init(&shared.broadcast_mutex, NULL),
                  "pthread_mutex_init broadcast");
    check_pthread(pthread_cond_init(&shared.broadcast_cond, NULL),
                  "pthread_cond_init broadcast");

    for (unsigned worker_id = 0; worker_id < config.workers; ++worker_id) {
        struct wake_worker *worker = &workers[worker_id];

        worker->shared = &shared;
        worker->worker_id = worker_id;
        worker->samples =
            xcalloc(config.rounds, sizeof(*worker->samples), "wake samples");

        check_pthread(pthread_mutex_init(&worker->mutex, NULL),
                      "pthread_mutex_init wake worker");
        check_pthread(pthread_cond_init(&worker->cond, NULL),
                      "pthread_cond_init wake worker");

        check_pthread(
            pthread_create(
                &threads[worker_id],
                NULL,
                config.wake_pattern == WAKE_FANOUT
                    ? wake_fanout_worker
                    : wake_broadcast_worker,
                worker),
            "pthread_create wake worker");
    }

    wait_for_workers_ready(&shared.barrier, config.workers);

    const unsigned total_iterations =
        config.warmup_rounds + config.rounds;
    uint64_t start_ns = 0;

    for (unsigned iteration = 0; iteration < total_iterations; ++iteration) {
        if (iteration == config.warmup_rounds) {
            start_ns = monotonic_ns();
        }

        begin_round(&shared.barrier);
        const uint64_t round_start_ns = monotonic_ns();

        if (config.wake_pattern == WAKE_FANOUT) {
            for (unsigned worker_id = 0; worker_id < config.workers; ++worker_id) {
                struct wake_worker *worker = &workers[worker_id];

                check_pthread(pthread_mutex_lock(&worker->mutex),
                              "pthread_mutex_lock fanout publish");
                worker->round_start_ns = round_start_ns;
                worker->publish_ns = monotonic_ns();
                ++worker->sequence;
                check_pthread(pthread_cond_signal(&worker->cond),
                              "pthread_cond_signal fanout");
                check_pthread(pthread_mutex_unlock(&worker->mutex),
                              "pthread_mutex_unlock fanout publish");
            }
        } else {
            check_pthread(pthread_mutex_lock(&shared.broadcast_mutex),
                          "pthread_mutex_lock broadcast publish");
            shared.broadcast_publish_ns = round_start_ns;
            ++shared.broadcast_sequence;
            check_pthread(pthread_cond_broadcast(&shared.broadcast_cond),
                          "pthread_cond_broadcast wake");
            check_pthread(pthread_mutex_unlock(&shared.broadcast_mutex),
                          "pthread_mutex_unlock broadcast publish");
        }

        wait_for_round_completion(&shared.barrier, config.workers);
    }

    const uint64_t wall_ns = monotonic_ns() - start_ns;

    if (config.wake_pattern == WAKE_FANOUT) {
        for (unsigned worker_id = 0; worker_id < config.workers; ++worker_id) {
            struct wake_worker *worker = &workers[worker_id];

            check_pthread(pthread_mutex_lock(&worker->mutex),
                          "pthread_mutex_lock fanout stop");
            worker->stop = true;
            check_pthread(pthread_cond_signal(&worker->cond),
                          "pthread_cond_signal fanout stop");
            check_pthread(pthread_mutex_unlock(&worker->mutex),
                          "pthread_mutex_unlock fanout stop");
        }
    }

    for (unsigned worker_id = 0; worker_id < config.workers; ++worker_id) {
        check_pthread(pthread_join(threads[worker_id], NULL),
                      "pthread_join wake worker");
    }

    const int result = print_wake_result(&shared, workers, wall_ns);

    for (unsigned worker_id = 0; worker_id < config.workers; ++worker_id) {
        check_pthread(pthread_cond_destroy(&workers[worker_id].cond),
                      "pthread_cond_destroy wake worker");
        check_pthread(pthread_mutex_destroy(&workers[worker_id].mutex),
                      "pthread_mutex_destroy wake worker");
        free(workers[worker_id].samples);
    }

    check_pthread(pthread_cond_destroy(&shared.broadcast_cond),
                  "pthread_cond_destroy broadcast");
    check_pthread(pthread_mutex_destroy(&shared.broadcast_mutex),
                  "pthread_mutex_destroy broadcast");
    barrier_destroy(&shared.barrier);

    free(threads);
    free(workers);

    if (allow_all_benchmark_cpus(config.cpus) != 0) {
        fatal_errno("restore benchmark process affinity");
    }

    return result;
}

static uint8_t working_set_value(unsigned worker_id,
                                 unsigned epoch,
                                 size_t offset)
{
    return (uint8_t)(worker_id * 29U
                   + epoch * 17U
                   + (unsigned)(offset / 64U));
}

static void initialize_working_set(struct migration_worker *worker)
{
    for (size_t offset = 0;
         offset < worker->working_set_bytes;
         offset += 64) {
        worker->working_set[offset] =
            working_set_value(worker->worker_id, 0, offset);
    }
}

static void verify_and_advance_working_set(struct migration_worker *worker,
                                           unsigned epoch)
{
    for (size_t offset = 0;
         offset < worker->working_set_bytes;
         offset += 64) {
        const uint8_t expected =
            working_set_value(worker->worker_id, epoch, offset);

        if (worker->working_set[offset] != expected) {
            ++worker->data_errors;
        }

        worker->working_set[offset] =
            working_set_value(worker->worker_id, epoch + 1, offset);
        worker->checksum += worker->working_set[offset];
    }
}

static struct migration_sample perform_migration_iteration(
    struct migration_worker *worker,
    unsigned epoch)
{
    const struct bench_config *config = &worker->shared->config;

    int source_cpu = sched_getcpu();
    unsigned target_cpu;

    if (source_cpu < 0 || (unsigned)source_cpu >= config->cpus) {
        target_cpu = (worker->worker_id + epoch + 1) % config->cpus;
    } else {
        target_cpu = ((unsigned)source_cpu + 1) % config->cpus;
    }

    if (config->migration_trace_every != 0
        && epoch % config->migration_trace_every == 0) {
        printf("SCHEDBENCH_TRACE kind=migrate worker=%u epoch=%u stage=before_setaffinity source_cpu=%d target_cpu=%u\n",
               worker->worker_id,
               epoch,
               source_cpu,
               target_cpu);
    }

    const uint64_t request_ns = monotonic_ns();
    const int setaffinity_result = pin_current_to_cpu(target_cpu);
    const int affinity_errno = setaffinity_result == 0 ? 0 : errno;
    const uint64_t return_ns = monotonic_ns();

    if (config->migration_trace_every != 0
        && epoch % config->migration_trace_every == 0) {
        printf("SCHEDBENCH_TRACE kind=migrate worker=%u epoch=%u stage=after_setaffinity result=%d errno=%d\n",
               worker->worker_id,
               epoch,
               setaffinity_result,
               affinity_errno);
    }

    int observed_cpu = sched_getcpu();
    bool timed_out = false;

    if (setaffinity_result == 0) {
        const uint64_t deadline_ns =
            request_ns
            + (uint64_t)config->migration_timeout_us * UINT64_C(1000);

        while (observed_cpu != (int)target_cpu
               && monotonic_ns() < deadline_ns) {
            sched_yield();
            observed_cpu = sched_getcpu();
        }
        timed_out = observed_cpu != (int)target_cpu;
    } else {
        ++worker->affinity_errors;
        timed_out = true;
    }

    const uint64_t observed_ns = monotonic_ns();

    if (config->migration_trace_every != 0
        && epoch % config->migration_trace_every == 0) {
        printf("SCHEDBENCH_TRACE kind=migrate worker=%u epoch=%u stage=observed observed_cpu=%d timed_out=%u\n",
               worker->worker_id,
               epoch,
               observed_cpu,
               timed_out);
    }
    verify_and_advance_working_set(worker, epoch);

    return (struct migration_sample){
        .setaffinity_latency_ns = return_ns - request_ns,
        .completion_latency_ns = observed_ns - request_ns,
        .source_cpu = source_cpu,
        .target_cpu = (int)target_cpu,
        .observed_cpu = observed_cpu,
        .affinity_errno = affinity_errno,
        .timed_out = timed_out,
    };
}

static void *migration_worker_entry(void *opaque)
{
    struct migration_worker *worker = opaque;
    struct migration_shared *shared = worker->shared;
    const struct bench_config *config = &shared->config;
    const bool wave = config->migration_pattern == MIGRATE_WAVE;

    const unsigned initial_cpu = worker->worker_id % config->cpus;
    if (config->migration_trace_every != 0) {
        printf("SCHEDBENCH_TRACE kind=migrate worker=%u stage=before_initial_pin source_cpu=%d target_cpu=%u\n",
               worker->worker_id,
               sched_getcpu(),
               initial_cpu);
    }
    worker->setup_error =
        pin_current_and_wait(initial_cpu, config->migration_timeout_us);

    if (config->migration_trace_every != 0) {
        printf("SCHEDBENCH_TRACE kind=migrate worker=%u stage=after_initial_pin setup_error=%d observed_cpu=%d\n",
               worker->worker_id,
               worker->setup_error,
               sched_getcpu());
    }

    worker_mark_ready(&shared->barrier);
    wait_for_benchmark_start(&shared->barrier);

    for (unsigned warmup = 0;
         warmup < config->warmup_rounds;
         ++warmup) {
        (void)perform_migration_iteration(worker, warmup);

        if (wave) {
            generation_barrier_wait(&shared->generation_barrier,
                                    config->workers,
                                    NULL);
        }
    }

    /*
     * This one barrier is intentional even in independent mode: it excludes
     * setup and warmup from wall-clock throughput without synchronizing the
     * measured independent iterations.
     */
    generation_barrier_wait(&shared->generation_barrier,
                            config->workers,
                            &shared->measurement_start_ns);

    for (unsigned round = 0; round < config->rounds; ++round) {
        const unsigned epoch = config->warmup_rounds + round;
        worker->samples[round] =
            perform_migration_iteration(worker, epoch);

        if (wave) {
            generation_barrier_wait(&shared->generation_barrier,
                                    config->workers,
                                    NULL);
        }
    }

    return NULL;
}

static int print_migration_result(const struct migration_shared *shared,
                                  const struct migration_worker *workers,
                                  uint64_t wall_ns)
{
    const struct bench_config *config = &shared->config;
    const size_t sample_count = (size_t)config->workers * config->rounds;

    uint64_t *setaffinity_latencies =
        xcalloc(sample_count, sizeof(*setaffinity_latencies),
                "setaffinity latencies");
    uint64_t *completion_latencies =
        xcalloc(sample_count, sizeof(*completion_latencies),
                "completion latencies");

    uint64_t cpu_hits[MAX_BENCH_CPUS] = { 0 };
    uint64_t checksum = 0;

    size_t setaffinity_count = 0;
    size_t completion_count = 0;
    size_t successful_count = 0;

    unsigned setup_errors = 0;
    unsigned affinity_errors = 0;
    unsigned target_mismatches = 0;
    unsigned timeout_count = 0;
    unsigned data_errors = 0;
    unsigned invalid_source_count = 0;

    for (unsigned worker_id = 0; worker_id < config->workers; ++worker_id) {
        const struct migration_worker *worker = &workers[worker_id];

        setup_errors += worker->setup_error != 0;
        affinity_errors += worker->affinity_errors;
        data_errors += worker->data_errors;
        checksum ^= worker->checksum;

        for (unsigned round = 0; round < config->rounds; ++round) {
            const struct migration_sample *sample = &worker->samples[round];

            if (sample->source_cpu < 0
                || (unsigned)sample->source_cpu >= config->cpus) {
                ++invalid_source_count;
            }

            if (sample->affinity_errno == 0) {
                setaffinity_latencies[setaffinity_count++] =
                    sample->setaffinity_latency_ns;
            }

            const bool matched =
                sample->observed_cpu == sample->target_cpu;

            target_mismatches += !matched;
            timeout_count += sample->timed_out;

            if (sample->affinity_errno == 0
                && !sample->timed_out
                && matched) {
                completion_latencies[completion_count++] =
                    sample->completion_latency_ns;
                ++successful_count;

                if (sample->observed_cpu >= 0
                    && (unsigned)sample->observed_cpu < config->cpus) {
                    ++cpu_hits[sample->observed_cpu];
                }
            }
        }
    }

    qsort(setaffinity_latencies,
          setaffinity_count,
          sizeof(*setaffinity_latencies),
          compare_u64);
    qsort(completion_latencies,
          completion_count,
          sizeof(*completion_latencies),
          compare_u64);

    for (unsigned cpu = 0; cpu < config->cpus; ++cpu) {
        printf("SCHEDBENCH_CPU kind=migrate pattern=%s cpu=%u "
               "successful_completions=%" PRIu64 "\n",
               config->migration_pattern == MIGRATE_WAVE
                   ? "wave"
                   : "independent",
               cpu,
               cpu_hits[cpu]);
    }

    const uint64_t setaffinity_p50 =
        percentile_per_mille(setaffinity_latencies,
                             setaffinity_count,
                             500);
    const uint64_t setaffinity_p90 =
        percentile_per_mille(setaffinity_latencies,
                             setaffinity_count,
                             900);
    const uint64_t setaffinity_p99 =
        percentile_per_mille(setaffinity_latencies,
                             setaffinity_count,
                             990);

    const uint64_t completion_p50 =
        percentile_per_mille(completion_latencies,
                             completion_count,
                             500);
    const uint64_t completion_p90 =
        percentile_per_mille(completion_latencies,
                             completion_count,
                             900);
    const uint64_t completion_p99 =
        percentile_per_mille(completion_latencies,
                             completion_count,
                             990);
    const uint64_t completion_p999 =
        percentile_per_mille(completion_latencies,
                             completion_count,
                             999);
    const uint64_t completion_max =
        completion_count == 0
            ? 0
            : completion_latencies[completion_count - 1];

    const double migrations_per_s =
        wall_ns == 0 ? 0.0 : (double)sample_count * 1e9 / wall_ns;
    const double completion_rate =
        sample_count == 0
            ? 0.0
            : (double)successful_count / sample_count;

    const char *pattern =
        config->migration_pattern == MIGRATE_WAVE
            ? "wave"
            : "independent";

    printf("SCHEDBENCH_RESULT kind=migrate pattern=%s workers=%u cpus=%u "
           "warmup=%u rounds=%u working_set_kib=%u samples=%zu "
           "wall_ns=%" PRIu64 " migrations_per_s=%.2f "
           "setaffinity_samples=%zu setaffinity_p50_ns=%" PRIu64
           " setaffinity_p90_ns=%" PRIu64
           " setaffinity_p99_ns=%" PRIu64
           " completion_samples=%zu completion_p50_ns=%" PRIu64
           " completion_p90_ns=%" PRIu64
           " completion_p99_ns=%" PRIu64
           " completion_p999_ns=%" PRIu64
           " completion_max_ns=%" PRIu64
           " successful_count=%zu target_mismatches=%u timeout_count=%u "
           "setup_errors=%u affinity_errors=%u data_errors=%u "
           "invalid_source_count=%u completion_rate=%.6f checksum=%" PRIu64 "\n",
           pattern,
           config->workers,
           config->cpus,
           config->warmup_rounds,
           config->rounds,
           config->working_set_kib,
           sample_count,
           wall_ns,
           migrations_per_s,
           setaffinity_count,
           setaffinity_p50,
           setaffinity_p90,
           setaffinity_p99,
           completion_count,
           completion_p50,
           completion_p90,
           completion_p99,
           completion_p999,
           completion_max,
           successful_count,
           target_mismatches,
           timeout_count,
           setup_errors,
           affinity_errors,
           data_errors,
           invalid_source_count,
           completion_rate,
           checksum);

    printf("SCHEDBENCH_HUMAN kind=migrate pattern=%s "
           "setaffinity_p50_us=%.3f setaffinity_p90_us=%.3f "
           "setaffinity_p99_us=%.3f completion_p50_us=%.3f "
           "completion_p90_us=%.3f completion_p99_us=%.3f "
           "completion_p999_us=%.3f completion_max_us=%.3f\n",
           pattern,
           ns_to_us(setaffinity_p50),
           ns_to_us(setaffinity_p90),
           ns_to_us(setaffinity_p99),
           ns_to_us(completion_p50),
           ns_to_us(completion_p90),
           ns_to_us(completion_p99),
           ns_to_us(completion_p999),
           ns_to_us(completion_max));

    free(completion_latencies);
    free(setaffinity_latencies);

    return setup_errors == 0
               && affinity_errors == 0
               && target_mismatches == 0
               && timeout_count == 0
               && data_errors == 0
               && invalid_source_count == 0
               && successful_count == sample_count
           ? EXIT_SUCCESS
           : EXIT_FAILURE;
}

static int run_migration_benchmark(struct bench_config config)
{
    struct migration_shared shared = { .config = config };
    struct migration_worker *workers =
        xcalloc(config.workers, sizeof(*workers), "migration workers");
    pthread_t *threads =
        xcalloc(config.workers, sizeof(*threads), "migration threads");

    const size_t working_set_bytes =
        (size_t)config.working_set_kib * 1024;

    barrier_init(&shared.barrier);
    generation_barrier_init(&shared.generation_barrier);

    for (unsigned worker_id = 0; worker_id < config.workers; ++worker_id) {
        struct migration_worker *worker = &workers[worker_id];

        worker->shared = &shared;
        worker->worker_id = worker_id;
        worker->samples =
            xcalloc(config.rounds,
                    sizeof(*worker->samples),
                    "migration samples");
        worker->working_set_bytes = working_set_bytes;
        worker->working_set =
            xcalloc(working_set_bytes == 0 ? 1 : working_set_bytes,
                    1,
                    "migration working set");

        initialize_working_set(worker);

        check_pthread(
            pthread_create(&threads[worker_id],
                           NULL,
                           migration_worker_entry,
                           worker),
            "pthread_create migration worker");
    }

    wait_for_workers_ready(&shared.barrier, config.workers);
    release_benchmark_start(&shared.barrier);

    for (unsigned worker_id = 0; worker_id < config.workers; ++worker_id) {
        check_pthread(pthread_join(threads[worker_id], NULL),
                      "pthread_join migration worker");
    }

    const uint64_t wall_ns =
        monotonic_ns() - shared.measurement_start_ns;
    const int result =
        print_migration_result(&shared, workers, wall_ns);

    for (unsigned worker_id = 0; worker_id < config.workers; ++worker_id) {
        free(workers[worker_id].working_set);
        free(workers[worker_id].samples);
    }

    generation_barrier_destroy(&shared.generation_barrier);
    barrier_destroy(&shared.barrier);

    free(threads);
    free(workers);
    return result;
}

static int run_wake_case(struct bench_config config,
                         enum wake_pattern pattern)
{
    config.kind = BENCH_WAKE;
    config.wake_pattern = pattern;

    const char *name =
        pattern == WAKE_FANOUT ? "fanout" : "broadcast";
    const enum watchdog_phase watchdog =
        pattern == WAKE_FANOUT
            ? WATCHDOG_WAKE_FANOUT
            : WATCHDOG_WAKE_BROADCAST;

    printf("SCHEDBENCH_PHASE kind=wake pattern=%s stage=begin\n", name);
    arm_watchdog(watchdog, config.phase_timeout_sec);
    const int result = run_wake_benchmark(config);
    disarm_watchdog();

    printf("SCHEDBENCH_CASE_%s kind=wake pattern=%s\n",
           result == EXIT_SUCCESS ? "PASSED" : "FAILED",
           name);
    return result;
}

static int run_migration_case(struct bench_config config,
                              enum migration_pattern pattern)
{
    config.kind = BENCH_MIGRATE;
    config.migration_pattern = pattern;

    const char *name =
        pattern == MIGRATE_WAVE ? "wave" : "independent";
    const enum watchdog_phase watchdog =
        pattern == MIGRATE_WAVE
            ? WATCHDOG_MIGRATE_WAVE
            : WATCHDOG_MIGRATE_INDEPENDENT;

    printf("SCHEDBENCH_PHASE kind=migrate pattern=%s stage=begin\n", name);
    arm_watchdog(watchdog, config.phase_timeout_sec);
    const int result = run_migration_benchmark(config);
    disarm_watchdog();

    printf("SCHEDBENCH_CASE_%s kind=migrate pattern=%s\n",
           result == EXIT_SUCCESS ? "PASSED" : "FAILED",
           name);
    return result;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (signal(SIGALRM, benchmark_timeout_handler) == SIG_ERR) {
        fatal_errno("signal(SIGALRM)");
    }

    const struct bench_config config = parse_config(argc, argv);
    const long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (online_cpus < 1
        || config.cpus > (unsigned long)online_cpus) {
        fprintf(stderr,
                "requested %u benchmark CPUs, but only %ld are online\n",
                config.cpus,
                online_cpus);
        return EXIT_FAILURE;
    }

    printf("SCHEDBENCH_BEGIN workers=%u cpus=%u warmup=%u rounds=%u "
           "phase_timeout_sec=%u\n",
           config.workers,
           config.cpus,
           config.warmup_rounds,
           config.rounds,
           config.phase_timeout_sec);

    bool passed = true;

    if (config.kind == BENCH_WAKE) {
        passed =
            run_wake_case(config, config.wake_pattern) == EXIT_SUCCESS;
    } else if (config.kind == BENCH_MIGRATE) {
        passed =
            run_migration_case(config, config.migration_pattern)
            == EXIT_SUCCESS;
    } else {
        passed &=
            run_wake_case(config, WAKE_FANOUT) == EXIT_SUCCESS;
        passed &=
            run_wake_case(config, WAKE_BROADCAST) == EXIT_SUCCESS;

        if (config.migration_pattern == MIGRATE_BOTH) {
            passed &=
                run_migration_case(config, MIGRATE_INDEPENDENT)
                == EXIT_SUCCESS;
            passed &=
                run_migration_case(config, MIGRATE_WAVE)
                == EXIT_SUCCESS;
        } else {
            passed &=
                run_migration_case(config, config.migration_pattern)
                == EXIT_SUCCESS;
        }
    }

    puts(passed ? "SCHEDBENCH_PASSED" : "SCHEDBENCH_FAILED");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
