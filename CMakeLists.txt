cmake_minimum_required(VERSION 3.10)
project(MemoryPoolLibrary LANGUAGES C CXX)

# C++17を有効化（std::pmrに必須）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C言語の規格設定
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# スレッドライブラリの検索 (pthread等)
find_package(Threads REQUIRED)

# ライブラリの定義
add_library(MemoryManager STATIC
    MemoryManager.cpp
    MemoryManager.hpp
    MemoryManager_C.h
)

# ライブラリがスレッドを使用することを明示
target_link_libraries(MemoryManager PRIVATE Threads::Threads)

# テスト実行ファイルの定義
add_executable(mem_test main.cpp)

# 実行ファイルにライブラリをリンク
target_link_libraries(mem_test PRIVATE MemoryManager)

# 警告レベルを上げて品質を確保（製品レベルのたしなみ）
if(MSVC)
    target_compile_options(MemoryManager PRIVATE /W4)
else()
    target_compile_options(MemoryManager PRIVATE -Wall -Wextra -Wpedantic)
endif()