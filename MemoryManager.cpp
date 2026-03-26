#include "MemoryManager.hpp"
#include "MemoryManager_C.h"

extern "C" {

int LabPool_Init(size_t size) {
    Lab::MemoryManager::Instance().Initialize(size);
    return 0; // 成功
}

void* LabPool_MallocTimeout(size_t size, int timeout_ms) {
    return Lab::MemoryManager::Instance().Allocate(size, timeout_ms);
}

void LabPool_Free(void* ptr) {
    Lab::MemoryManager::Instance().Deallocate(ptr);
}

}