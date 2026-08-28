# MicroTeX — движок формул: разбирает LaTeX в дерево боксов и рисует его
# вызовами абстрактного tex::Graphics2D. Взято ядро без каталога platform/ —
# три статические фабрики (Font::create, Font::_create, TextLayout::create)
# и сам Graphics2D реализует Typography поверх DirectWrite и Direct2D, см.
# Typography/src/formula.cpp. Какой срез взят и почему без vcpkg — README.md
# рядом.
#
# После удаления platform/ и samples/ ядро — это все .cpp поддерева целиком,
# поэтому GLOB здесь безопасен: состав меняется только новым вендорингом.
file(GLOB_RECURSE MICROTEX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/MicroTeX/src/*.cpp")

add_library(microtex STATIC
    ${MICROTEX_SOURCES}
    tinyxml2/tinyxml2.cpp
)

# SYSTEM: заголовки чужие, и их предупреждения в потребителях (/W4 у кода
# Буквицы) — не наши; MSVC получает их через /external:I с /external:W0.
target_include_directories(microtex SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/MicroTeX/src")
target_include_directories(microtex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tinyxml2")

# C++17 — родной стандарт MicroTeX под MSVC; двадцать третий всего дерева ему
# не нужен, а BMI это не касается — модулей у него нет.
set_target_properties(microtex PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Чужой код с чужими предупреждениями: они не адресованы этой сборке, как и
# предупреждения wxl (см. корневой CMakeLists про /W4).
if(MSVC)
    target_compile_options(microtex PRIVATE /w)
endif()

add_library(bukvitsa::microtex ALIAS microtex)
