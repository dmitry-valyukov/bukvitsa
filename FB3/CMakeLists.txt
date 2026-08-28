# Модуль C++20, а не библиотека заголовков: см. вводную статью в src/fb3.ixx.
# Машинерия — та же, что у модулей wxl (`wxl_add_module_library` из
# cmake/wxl_helpers.cmake, подключённого в корневом CMakeLists).
wxl_add_module_library(bukvitsa_fb3 STATIC
    PUBLIC
        src/fb3.ixx

        src/node.ixx
        src/description.ixx
        src/opc.ixx
        src/document.ixx
    PRIVATE
        # Внутренняя кухня разбора: первичный интерфейс её не переэкспортирует,
        # и потребителю модуля она не видна. `internal.ixx` — ещё и то место,
        # где живёт wxl::xml, о которой публичный вид FB3 не знает.
        src/parse_helpers.ixx
        src/internal.ixx
    SRC
        src/opc.cpp
        src/node.cpp
        src/description.cpp
        src/document.cpp
        src/parse_helpers.cpp
)
add_library(bukvitsa::fb3 ALIAS bukvitsa_fb3)

# PUBLIC, в отличие от прежнего PRIVATE: модель книги отвечает
# wxl::text::u8_view, то есть тип из wxl.text стоит в интерфейсе модуля, и
# компилировать его импорт приходится всякому, кто импортирует FB3.
target_link_libraries(bukvitsa_fb3 PUBLIC wxl::text)

# wxl::xml — только внутри: её импортирует партиция :internal, которую
# первичный интерфейс не переэкспортирует.
target_link_libraries(bukvitsa_fb3 PRIVATE wxl::xml)

if(WIN32)
    target_link_libraries(bukvitsa_fb3 PRIVATE ole32 oleaut32)  # Packaging API
endif()

if(BUKVITSA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
