add_library(bukvitsa_typography STATIC
    src/flatten.cpp
    src/linebreak.cpp
    src/engine.cpp
    src/formula.cpp
    src/glyph_painter.cpp
    src/chapter.cpp
    src/typography.cpp
)
add_library(bukvitsa::typography ALIAS bukvitsa_typography)

target_include_directories(bukvitsa_typography PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(bukvitsa_typography PUBLIC bukvitsa::fb3)

# wxl::text — только внутри: обход кодовых точек книги при развороте в блоки.
target_link_libraries(bukvitsa_typography PRIVATE wxl::text)

# MicroTeX — только внутри: наружу formula.h отдаёт Formula и FormulaEngine,
# ни одного имени tex:: в публичных заголовках нет.
target_link_libraries(bukvitsa_typography PRIVATE bukvitsa::microtex)
bukvitsa_consumes_wxl(bukvitsa_typography)
if(WIN32)
    target_link_libraries(bukvitsa_typography PUBLIC dwrite d2d1)
endif()

# TODO: переносы (hunspell-hyphen + словари hyph_ru_RU/hyph_en_US в external/) —
# в первой версии их нет по решению: см. docs/decisions.md.

if(BUKVITSA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
