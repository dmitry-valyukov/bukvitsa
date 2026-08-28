add_executable(typography_tests typography_tests.cpp)

# wxl::core — за STA-пулом: тест открывает настоящие книги, а разбор XML
# берёт из пула память.
target_link_libraries(typography_tests PRIVATE bukvitsa::typography wxl::core)
bukvitsa_consumes_wxl(typography_tests)

add_test(NAME typography_tests COMMAND typography_tests)

target_compile_definitions(typography_tests PRIVATE
    BUKVITSA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../FB3/testdata"
    BUKVITSA_MICROTEX_RES="${CMAKE_CURRENT_SOURCE_DIR}/../../external/MicroTeX/res")

# windowscodecs — CLSID фабрики WIC для растеризации формулы в тесте.
target_link_libraries(typography_tests PRIVATE windowscodecs)
