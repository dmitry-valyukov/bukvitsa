add_executable(mathml_tests mathml_tests.cpp)

# wxl::core — за STA-пулом: разбор XML берёт из него память.
target_link_libraries(mathml_tests PRIVATE bukvitsa::mathml wxl::core wxl::text)
bukvitsa_consumes_wxl(mathml_tests)

add_test(NAME mathml_tests COMMAND mathml_tests)
