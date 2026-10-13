/*
 * Replays a geometry-engine recording (the PC runner's --gx-record FILE)
 * with the engine on the caller's thread, then with it on a thread of its
 * own, and checks that both give the same scenes and the same results the
 * game reads back (matrix results, GXSTAT, test results).
 *
 *   gx3d_replay FILE
 *
 * Not part of `make test`: recordings come from the game, and stay on the
 * machine that made them (they're the game's data).
 */
#include <nitro_port_gx3d.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char NitroPort_IoReg[];

typedef struct {
    uint32_t *words;
    size_t count, capacity;
} Hashes;

static Hashes sHashes[2];
static int sPass;
static uint32_t sSeen;

static uint32_t hash_bytes(uint32_t h, const void *data, size_t size)
{
    const unsigned char *p = data;
    size_t i;

    for (i = 0; i < size; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Scenes come from the geometry thread, read-backs from the caller's. */
static pthread_mutex_t sHashLock = PTHREAD_MUTEX_INITIALIZER;
static void add_hash(uint32_t kind, uint32_t h)
{
    Hashes *list = &sHashes[sPass];

    pthread_mutex_lock(&sHashLock);

    if (list->count + 2 > list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 4096;
        list->words = realloc(list->words, list->capacity * sizeof(uint32_t));
        if (!list->words) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
    }
    list->words[list->count++] = kind;
    list->words[list->count++] = h;
    pthread_mutex_unlock(&sHashLock);
}

/* Each scene as it's published (on whichever thread publishes it). */
void NitroPort_Gx3dScenePublished(void)
{
    const NitroGx3dScene *scene = NitroPort_Gx3dAcquireScene(&sSeen);
    uint32_t h = 2166136261u;
    uint32_t i;

    if (!scene) {
        return;
    }
    h = hash_bytes(h, &scene->count, sizeof(scene->count));
    h = hash_bytes(h, &scene->dropped, sizeof(scene->dropped));
    h = hash_bytes(h, &scene->swap_param, sizeof(scene->swap_param));
    for (i = 0; i < scene->count; i++) {
        const NitroGx3dTriangle *tri = &scene->triangles[i];
        int k;

        for (k = 0; k < 3; k++) {
            const NitroGx3dVertex *v = &tri->v[k];

            h = hash_bytes(h, v->clip, sizeof(v->clip));
            h = hash_bytes(h, &v->x, sizeof(v->x));
            h = hash_bytes(h, &v->y, sizeof(v->y));
            h = hash_bytes(h, &v->depth, sizeof(v->depth));
            h = hash_bytes(h, &v->inv_w, sizeof(v->inv_w));
            h = hash_bytes(h, &v->s, sizeof(v->s));
            h = hash_bytes(h, &v->t, sizeof(v->t));
            h = hash_bytes(h, &v->color, sizeof(v->color));
        }
        h = hash_bytes(h, &tri->polygon_attr, sizeof(tri->polygon_attr));
        h = hash_bytes(h, &tri->texture_param, sizeof(tri->texture_param));
        h = hash_bytes(h, &tri->palette_base, sizeof(tri->palette_base));
        h = hash_bytes(h, &tri->sort_key, sizeof(tri->sort_key));
    }
    {
        /* GX3D_REPLAY_SHOW=N: what scene N has, in each pass. */
        static size_t number[2];
        const char *show = getenv("GX3D_REPLAY_SHOW");

        if (show && (size_t)atol(show) == number[sPass]) {
            printf("pass %d scene %zu: %u triangles, %u dropped, swap %08x\n", sPass,
                   number[sPass], scene->count, scene->dropped, scene->swap_param);
            for (i = 0; i < scene->count && i < 400; i++) {
                const NitroGx3dTriangle *tri = &scene->triangles[i];
                printf("  %u: attr %08x tex %08x key %05x v0 %d,%d c%04x clip %d %d %d %d\n", i,
                       tri->polygon_attr, tri->texture_param, tri->sort_key, tri->v[0].x,
                       tri->v[0].y, tri->v[0].color, tri->v[0].clip[0], tri->v[0].clip[1],
                       tri->v[0].clip[2], tri->v[0].clip[3]);
            }
        }
        number[sPass]++;
    }
    NitroPort_Gx3dReleaseScene(scene);
    add_hash(1, h);
}

#ifndef SYNC_ONLY   /* SYNC_ONLY: for --hashes against an engine without the thread */
static sem_t sWorkerSem, sProducerSem;
static volatile int sRunning = 1;
static void wait_worker(void *c) { (void)c; while (sem_wait(&sWorkerSem) != 0) {} }
static void wake_worker(void *c) { (void)c; sem_post(&sWorkerSem); }
static void wait_producer(void *c) { (void)c; while (sem_wait(&sProducerSem) != 0) {} }
static void wake_producer(void *c) { (void)c; sem_post(&sProducerSem); }
static void *geometry_thread(void *unused)
{
    (void)unused;
    NitroPort_Gx3dThreadMain(&sRunning);
    return NULL;
}
#endif

/* What the game reads back: GXSTAT's engine bits, the matrix and test
 * result registers. */
static uint32_t results_hash(void)
{
    uint32_t status, h = 2166136261u;

    memcpy(&status, NitroPort_IoReg + 0x600, sizeof(status));
    status &= (3u << 0) | (31u << 8) | (1u << 13) | (1u << 14) | (1u << 15);
    h = hash_bytes(h, &status, sizeof(status));
    h = hash_bytes(h, NitroPort_IoReg + 0x620, 0x6a4 - 0x620);
    return h;
}

static void replay(const uint32_t *data, size_t words)
{
    size_t at = 0;

    sSeen = 0;
    NitroPort_Gx3dReset();
    while (at + 2 <= words) {
        uint32_t type = data[at], count = data[at + 1];
        const uint32_t *p = &data[at + 2];

        if (at + 2 + count > words) {
            break;
        }
        switch (type) {
        case 1: NitroPort_GxFifoWords(p, count); break;
        case 3: NitroPort_GxDirectWrite((int)p[0], p[1]); break;
        case 4: NitroPort_Gx3dClearStackError(); break;
        case 5: NitroPort_Gx3dSyncResults(); add_hash(2, results_hash()); break;
        case 6: {
            uint32_t status;

            NitroPort_Gx3dRefreshStatus();
            memcpy(&status, NitroPort_IoReg + 0x600, sizeof(status));
            /* The box test result, stack levels and overflow. */
            add_hash(3, status & ((3u << 0) | (31u << 8) | (1u << 13) | (1u << 14) | (1u << 15)));
            break;
        }
        case 7: break;  /* the replay starts from a reset engine */
        default:
            fprintf(stderr, "unknown record %u at word %zu\n", type, at);
            exit(2);
        }
        at += 2 + count;
    }
    NitroPort_Gx3dScene();   /* everything done */
}

int main(int argc, char **argv)
{
    static const char *kinds[] = { "?", "scene", "matrix results", "GXSTAT" };
#ifndef SYNC_ONLY
    NitroGx3dThreadOps ops;
    pthread_t thread;
#endif
    FILE *f;
    long size;
    uint32_t *data;
    size_t i, scenes = 0;

    const char *path = argv[argc - 1];
    int hashesOnly = argc == 3 && strcmp(argv[1], "--hashes") == 0;

    if (argc != 2 && !hashesOnly) {
        fprintf(stderr, "usage: %s [--hashes] RECORDING\n"
                "  --hashes: each scene's hash, without the thread (to compare versions)\n", argv[0]);
        return 2;
    }
    f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fprintf(stderr, "can't read %s\n", path);
        return 2;
    }
    rewind(f);
    data = malloc((size_t)size + 4);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "can't read %s\n", path);
        return 2;
    }
    fclose(f);

    sPass = 0;
    replay(data, (size_t)size / 4);
    if (hashesOnly) {
        for (i = 0; i < sHashes[0].count; i += 2) {
            if (sHashes[0].words[i] == 1) {
                printf("%08x\n", sHashes[0].words[i + 1]);
            }
        }
        return 0;
    }
#ifndef SYNC_ONLY

    memset(&ops, 0, sizeof(ops));
    ops.wait_worker = wait_worker;
    ops.wake_worker = wake_worker;
    ops.wait_producer = wait_producer;
    ops.wake_producer = wake_producer;
    ops.keep_every_scene = 1;
    sem_init(&sWorkerSem, 0, 0);
    sem_init(&sProducerSem, 0, 0);
    NitroPort_Gx3dStartThread(&ops);
    pthread_create(&thread, NULL, geometry_thread, NULL);
    sPass = 1;
    replay(data, (size_t)size / 4);
    NitroPort_Gx3dStopThread();
    sRunning = 0;
    wake_worker(NULL);
    pthread_join(thread, NULL);

    for (i = 0; i < sHashes[0].count; i += 2) {
        if (sHashes[0].words[i] == 1) {
            scenes++;
        }
    }
    printf("%zu checks (%zu scenes)\n", sHashes[0].count / 2, scenes);
    /* Scenes and read-backs interleave differently with a thread: compare
     * each kind on its own. */
    for (i = 1; i <= 3; i++) {
        size_t a = 0, b = 0, n = 0;

        for (;;) {
            while (a < sHashes[0].count && sHashes[0].words[a] != i) a += 2;
            while (b < sHashes[1].count && sHashes[1].words[b] != i) b += 2;
            if (a >= sHashes[0].count || b >= sHashes[1].count) {
                break;
            }
            if (sHashes[0].words[a + 1] != sHashes[1].words[b + 1]) {
                printf("DIFFERENT: %s %zu (%08x, with the thread %08x)\n", kinds[i], n,
                       sHashes[0].words[a + 1], sHashes[1].words[b + 1]);
                return 1;
            }
            a += 2;
            b += 2;
            n++;
        }
        if ((a < sHashes[0].count) != (b < sHashes[1].count)) {
            printf("DIFFERENT: %s count\n", kinds[i]);
            return 1;
        }
        printf("%s: %zu the same\n", kinds[i], n);
    }
#endif
    return 0;
}
