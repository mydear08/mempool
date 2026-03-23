#include "MemoryManager.hpp"
#include "MemoryManager_C.h"

extern "C" {

int LabPool_Init(void* buffer, size_t size) {
    return (Lab::MemoryManager::Instance().Initialize(buffer, size) == Lab::MemoryManager::Result::Success);
}

void LabPool_Shutdown() {
    Lab::MemoryManager::Instance().Shutdown();
}

void* LabPool_MallocTimeout(size_t size, int timeout_ms) {
    return Lab::MemoryManager::Instance().Allocate(size, timeout_ms);
}

void LabPool_Free(void* ptr) {
    Lab::MemoryManager::Instance().Deallocate(ptr);
}

}