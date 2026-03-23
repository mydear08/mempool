#include "MemoryManager_C.h"
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define POOL_SIZE (300 * 1024 * 1024)
#define THREAD_COUNT 4  // i.MX 8のコア数に合わせて調整
#define ALLOC_SIZE (80 * 1024 * 1024) // 80MB x 4 = 320MB (プール容量300MBを超える設定)

static uint8_t system_heap[POOL_SIZE] __attribute__((aligned(64)));

void* worker_thread(void* arg) {
    int id = *(int*)arg;
    printf("Thread %d: Starting...\n", id);

    // 1. タイムアウト付きで確保を試みる (5秒待機)
    // 4スレッド合計で320MB要求するため、必ず誰かが待たされる計算
    void* ptr = LabPool_MallocTimeout(ALLOC_SIZE, 5000);

    if (ptr) {
        printf("Thread %d: Allocated 80MB at %p\n", id, ptr);
        memset(ptr, 0xAA, ALLOC_SIZE); // 実際に書き込んでメモリを物理的に触る
        
        sleep(2); // メモリを保持して他を待たせる

        LabPool_Free(ptr);
        printf("Thread %d: Released memory.\n", id);
    } else {
        printf("Thread %d: Allocation FAILED (Timeout as expected).\n", id);
    }

    return NULL;
}

int main() {
    if (!LabPool_Init(system_heap, sizeof(system_heap))) {
        printf("Init Failed!\n");
        return -1;
    }

    pthread_t threads[THREAD_COUNT];
    int thread_ids[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]);
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("Test completed.\n");
    return 0;
}