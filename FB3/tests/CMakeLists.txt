add_executable(fb3_tests fb3_tests.cpp)

# wxl::core, а не wxl::xml: тесту нужен только STA-пул (`import wxl.core;`),
# разбор XML он видит через FB3.
target_link_libraries(fb3_tests PRIVATE bukvitsa::fb3 wxl::core)
bukvitsa_consumes_wxl(fb3_tests)

add_test(NAME fb3_tests COMMAND fb3_tests)

target_compile_definitions(fb3_tests PRIVATE
    BUKVITSA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../testdata")

# Замер: сколько стоит узнать о книге то, что нужно витрине. Не тест —
# запускается руками, когда решается, где скану жить.
add_executable(fb3_bench fb3_bench.cpp)
target_link_libraries(fb3_bench PRIVATE bukvitsa::fb3 wxl::core)
bukvitsa_consumes_wxl(fb3_bench)
target_compile_definitions(fb3_bench PRIVATE
    BUKVITSA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../testdata")
