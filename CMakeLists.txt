cmake_minimum_required(VERSION 3.28)

project(vms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-fno-exceptions -Wall -Wextra -Wpedantic)

# 실행 파일과 테스트가 함께 링크하는 서버 구성 요소
add_library(server_core STATIC
    src/core/event_loop.cpp
    src/core/termination_signal_watcher.cpp
    src/http/http_request.cpp
    src/http/http_response.cpp
    src/http/http_server.cpp
    src/http/mime_type.cpp
    src/http/static_files.cpp
)
target_include_directories(server_core PUBLIC src)

add_executable(vms_web_server src/apps/web_server/main.cpp)
target_link_libraries(vms_web_server PRIVATE server_core)

enable_testing()
add_subdirectory(tests)
