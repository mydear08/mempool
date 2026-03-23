#ifndef MEMORY_MANAGER_C_H
#define MEMORY_MANAGER_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初期化。成功なら1、失敗なら0を返すわ。
int LabPool_Init(void* buffer, size_t size);

// 終了処理。デストラクタを明示的に呼び出してリソースを解放するわ。
void LabPool_Shutdown();

// タイムアウト付き確保。timeout_ms: 0なら即時、-1なら無限待機。
void* LabPool_MallocTimeout(size_t size, int timeout_ms);

// 通常の確保（タイムアウトなし）
static inline void* LabPool_Malloc(size_t size) {
    return LabPool_MallocTimeout(size, 0);
}

// 解放。
void LabPool_Free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif