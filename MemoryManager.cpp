#include "MemoryManager.hpp"
#include "MemoryManager_C.h"
#include <new>

namespace Lab {

// シングルトンインスタンスの実体（static変数による遅延初期化）
// MemoryManager::Instance() 内で保持されるため、ここでは不要。
// ただし、配置newで使用するポインタなどの管理はInstanceが行う。

} // namespace Lab

extern "C" {

/**
 * Cインターフェース: 初期化
 */
int LabPool_Init(void* buffer, size_t size) {
    auto res = Lab::MemoryManager::Instance().Initialize(buffer, size);
    return (res == Lab::MemoryManager::Result::Success) ? 1 : 0;
}

/**
 * Cインターフェース: タイムアウト付き確保
 */
void* LabPool_MallocTimeout(size_t size, int timeout_ms) {
    // MemoryManager内部で例外キャッチとタイムアウト処理が行われる
    return Lab::MemoryManager::Instance().Allocate(size, timeout_ms);
}

/**
 * Cインターフェース: 解放
 */
void LabPool_Free(void* ptr) {
    if (ptr == NULL) return;
    Lab::MemoryManager::Instance().Deallocate(ptr);
}

} // extern "C"