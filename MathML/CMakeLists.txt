# Модуль C++20 по той же машинерии, что FB3 и модули wxl.
wxl_add_module_library(bukvitsa_mathml STATIC
    PUBLIC
        src/mathml.ixx
    SRC
        src/mathml.cpp
)
add_library(bukvitsa::mathml ALIAS bukvitsa_mathml)

# PUBLIC: вход toTex — wxl::text::u8_view, тип из wxl.text стоит в интерфейсе.
target_link_libraries(bukvitsa_mathml PUBLIC wxl::text)

# wxl::xml — только внутри: разбор MathML не виден из интерфейса модуля.
target_link_libraries(bukvitsa_mathml PRIVATE wxl::xml)

if(BUKVITSA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
