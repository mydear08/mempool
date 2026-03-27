#include "MemoryManager.hpp"
#include "MemoryManager_C.h"

#include <iostream>

extern "C" {

int LabPool_Init(size_t size) {
    lab::MemoryManager::Instance().Initialize(size);
    return 0; // 成功
}

void* LabPool_MallocTimeout(size_t size, int timeout_ms) {
    return lab::MemoryManager::Instance().Allocate(size, timeout_ms);
}

void LabPool_Free(void* ptr) {
    lab::MemoryManager::Instance().Deallocate(ptr);
}

}