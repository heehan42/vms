# 실제 서버를 띄워 http-server 명세의 시나리오를 요청으로 검사한다.
add_test(NAME http_server_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/http_server_test.sh $<TARGET_FILE:vms_web_server>)
