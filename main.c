#include "MemoryManager_C.h"
#include <stdio.h>
#include <stdint.h>

// 64KBの静的バッファを用意
static uint8_t system_heap[64 * 1024] __attribute__((aligned(16)));

int main() {
    // 1. 初期化
    if (!LabPool_Init(system_heap, sizeof(system_heap))) {
        printf("Failed to initialize memory pool.\n");
        return -1;
    }

    // 2. 確保（タイムアウト 100ms）
    void* ptr = LabPool_MallocTimeout(1024, 100);
    
    if (ptr) {
        printf("Memory allocated at: %p\n", ptr);
        
        // 3. 解放
        LabPool_Free(ptr);
        printf("Memory freed.\n");
    } else {
        printf("Allocation failed (timeout or OOM).\n");
    }

    return 0;
}