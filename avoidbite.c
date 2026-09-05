/*
 * avoidbite — 旱厕蹲坑算法 (Squat-Pit, Avoid-the-Bite Algorithm)
 *
 * 主进程占据坑位开始蹲坑；蚊子（线程）不断袭扰，
 * 每叮一口就推动进度，袭扰越猛，蹲坑越快结束。
 * 叮咬收益随累计叮咬递减，蚊子过密时互相干扰。
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PROGRESS_GOAL     100
#define NUM_MOSQUITOES    5
#define BASE_TICK_MS      80
#define BASE_PROGRESS     1
#define BITE_BASE         4
#define BITE_FLOOR        1
#define INTERFERE_THRESH  8
#define BAR_WIDTH         40
#define MAX_MOSQUITOES    64

typedef struct {
    int progress;
    int done;
    int bites;
    int n_mosq;
    int ui_progress;
    int ui_bites;
    pthread_mutex_t lock;
    pthread_mutex_t ui_lock;
    pthread_cond_t  cv;
} Pit;

typedef struct {
    Pit *pit;
    int  id;
    unsigned seed;
} MosquitoArgs;

typedef struct {
    int progress;
    int bites;
    int done;
} PitSnapshot;

static void msleep(int ms)
{
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static double elapsed_sec(const struct timespec *start, const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec)
         + (end->tv_nsec - start->tv_nsec) / 1e9;
}

/* 叮咬收益：累计越多越弱；蚊子过密时互相干扰再打折 */
static int bite_delta(const Pit *pit)
{
    int decay = pit->bites / 10;
    int delta = BITE_BASE - decay;
    if (delta < BITE_FLOOR)
        delta = BITE_FLOOR;

    if (pit->n_mosq > INTERFERE_THRESH) {
        int overcrowded = pit->n_mosq - INTERFERE_THRESH;
        /* ceil(overcrowded/4)：超额从第 1 只起就生效，避免整除把 1–3 吃掉 */
        delta -= (overcrowded + 3) / 4;
        if (delta < BITE_FLOOR)
            delta = BITE_FLOOR;
    }
    return delta;
}

static void draw_bar(Pit *pit, int progress, int bites)
{
    int filled = (progress * BAR_WIDTH) / PROGRESS_GOAL;
    if (filled > BAR_WIDTH)
        filled = BAR_WIDTH;

    pthread_mutex_lock(&pit->ui_lock);
    /* 丢弃过期快照，避免并发刷新时进度条回跳 */
    if (progress < pit->ui_progress
        || (progress == pit->ui_progress && bites < pit->ui_bites)) {
        pthread_mutex_unlock(&pit->ui_lock);
        return;
    }
    pit->ui_progress = progress;
    pit->ui_bites = bites;

    printf("\r蹲坑 [");
    for (int i = 0; i < BAR_WIDTH; i++)
        putchar(i < filled ? '#' : '.');
    printf("] %3d%%  蚊子叮咬: %d", progress, bites);
    fflush(stdout);
    pthread_mutex_unlock(&pit->ui_lock);
}

static PitSnapshot snapshot_unlocked(const Pit *pit)
{
    PitSnapshot s = {
        .progress = pit->progress,
        .bites    = pit->bites,
        .done     = pit->done,
    };
    return s;
}

static void mark_done_unlocked(Pit *pit)
{
    if (pit->progress > PROGRESS_GOAL)
        pit->progress = PROGRESS_GOAL;
    pit->done = 1;
    pthread_cond_broadcast(&pit->cv);
}

/* 蚊子：定时叮咬；完成后被 broadcast 叫醒退出 */
static void *mosquito(void *arg)
{
    MosquitoArgs *ma = arg;
    Pit *pit = ma->pit;

    pthread_mutex_lock(&pit->lock);
    while (!pit->done) {
        int delay_ms = 30 + (rand_r(&ma->seed) % 120);
        struct timespec abs;
        clock_gettime(CLOCK_REALTIME, &abs);
        abs.tv_sec  += delay_ms / 1000;
        abs.tv_nsec += (long)(delay_ms % 1000) * 1000000L;
        if (abs.tv_nsec >= 1000000000L) {
            abs.tv_sec++;
            abs.tv_nsec -= 1000000000L;
        }

        int rc = 0;
        while (!pit->done && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&pit->cv, &pit->lock, &abs);

        if (pit->done)
            break;

        pit->progress += bite_delta(pit);
        pit->bites++;
        if (pit->progress >= PROGRESS_GOAL)
            mark_done_unlocked(pit);

        PitSnapshot snap = snapshot_unlocked(pit);
        pthread_mutex_unlock(&pit->lock);
        draw_bar(pit, snap.progress, snap.bites);
        pthread_mutex_lock(&pit->lock);
    }
    pthread_mutex_unlock(&pit->lock);

    return NULL;
}

/* 蹲坑者：自身慢推进；蚊子叮咬时也会刷新进度条 */
static void squat(Pit *pit)
{
    while (1) {
        msleep(BASE_TICK_MS);

        pthread_mutex_lock(&pit->lock);
        if (pit->done) {
            PitSnapshot snap = snapshot_unlocked(pit);
            pthread_mutex_unlock(&pit->lock);
            draw_bar(pit, snap.progress, snap.bites);
            break;
        }

        pit->progress += BASE_PROGRESS;
        if (pit->progress >= PROGRESS_GOAL)
            mark_done_unlocked(pit);

        PitSnapshot snap = snapshot_unlocked(pit);
        pthread_mutex_unlock(&pit->lock);
        draw_bar(pit, snap.progress, snap.bites);

        if (snap.done)
            break;
    }
}

static void join_mosq(pthread_t *threads, int n)
{
    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);
}

static int parse_count(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > MAX_MOSQUITOES)
        return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char *argv[])
{
    int n_mosq = NUM_MOSQUITOES;

    if (argc > 1) {
        if (parse_count(argv[1], &n_mosq) != 0) {
            fprintf(stderr, "用法: %s [蚊子数量 0-%d]\n", argv[0], MAX_MOSQUITOES);
            return 1;
        }
    }

    Pit pit = {
        .progress    = 0,
        .done        = 0,
        .bites       = 0,
        .n_mosq      = n_mosq,
        .ui_progress = -1,
        .ui_bites    = -1,
    };
    pthread_mutex_init(&pit.lock, NULL);
    pthread_mutex_init(&pit.ui_lock, NULL);
    pthread_cond_init(&pit.cv, NULL);

    printf("=== 旱厕蹲坑算法 ===\n");
    printf("坑位已占据，开始蹲坑...\n");
    printf("蚊子数量: %d（叮咬加速；过密会互相干扰）\n\n", n_mosq);

    pthread_t *threads = NULL;
    MosquitoArgs *args = NULL;
    int launched = 0;

    if (n_mosq > 0) {
        threads = calloc((size_t)n_mosq, sizeof(pthread_t));
        args    = calloc((size_t)n_mosq, sizeof(MosquitoArgs));
        if (!threads || !args) {
            fprintf(stderr, "内存分配失败\n");
            free(threads);
            free(args);
            pthread_mutex_destroy(&pit.lock);
            pthread_mutex_destroy(&pit.ui_lock);
            pthread_cond_destroy(&pit.cv);
            return 1;
        }

        for (int i = 0; i < n_mosq; i++) {
            args[i].pit  = &pit;
            args[i].id   = i + 1;
            args[i].seed = (unsigned)time(NULL) ^ (unsigned)(i * 2654435761u)
                         ^ (unsigned)args[i].id;
            if (pthread_create(&threads[i], NULL, mosquito, &args[i]) != 0) {
                fprintf(stderr, "蚊子 %d 起飞失败\n", i + 1);
                pthread_mutex_lock(&pit.lock);
                mark_done_unlocked(&pit);
                pthread_mutex_unlock(&pit.lock);
                join_mosq(threads, launched);
                free(threads);
                free(args);
                pthread_mutex_destroy(&pit.lock);
                pthread_mutex_destroy(&pit.ui_lock);
                pthread_cond_destroy(&pit.cv);
                return 1;
            }
            launched++;
        }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    squat(&pit);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* 先 join，再读最终 bites，避免与蚊子线程并发访问 */
    if (n_mosq > 0) {
        join_mosq(threads, launched);
        free(threads);
        free(args);
    }

    printf("\n\n蹲坑完成！共被叮 %d 口，耗时 %.2f 秒。\n",
           pit.bites, elapsed_sec(&t0, &t1));

    pthread_mutex_destroy(&pit.lock);
    pthread_mutex_destroy(&pit.ui_lock);
    pthread_cond_destroy(&pit.cv);

    printf("离坑。\n");
    return 0;
}
