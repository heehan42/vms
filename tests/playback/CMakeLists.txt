# 재생 모듈의 단위 테스트. server_core에 더해 playback 라이브러리를 링크한다.
function(add_playback_test name)
    add_unit_test(${name})
    target_link_libraries(${name} PRIVATE playback)
endfunction()

add_playback_test(recording_library_test)
add_playback_test(hls_playlist_test)
add_playback_test(recording_playback_endpoints_test)
add_playback_test(source_list_test)
