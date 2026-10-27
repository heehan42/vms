# tests/는 src/와 같은 디렉터리 구조를 따른다.
# 모듈별 단위 테스트는 같은 이름의 하위 디렉터리에, 실제 서버를 띄우는 테스트는 integration/에 둔다.

# 호출한 디렉터리의 <name>.cpp를 server_core와 링크한 단위 테스트 실행 파일로 만들고 CTest에 등록한다.
function(add_unit_test name)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
    target_link_libraries(${name} PRIVATE server_core)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_subdirectory(core)
add_subdirectory(crypto)
add_subdirectory(http)
add_subdirectory(webrtc)
add_subdirectory(integration)
