# ---- Сайт документации Буквицы ----
#
#   tools\build.ps1 -Target docs
#   build\x64\docs\bukvitsa\html\index.html
#
# Машинерия — общая с wxl (`cmake/wxl_doxygen.cmake` в её дереве), потому что
# заводить второе место, где живут версия Doxygen, тема и вид Doxyfile, значило
# бы чинить каждую находку дважды. Буквица подключает её ровно так же, как уже
# подключает `vcpkg_bootstrap.cmake` и `wxl_helpers.cmake`.
#
# Здесь две цели, и это следствие принятого решения о направлении ссылок:
# сайт wxl самодостаточен и о Буквице не знает, а Буквица на него ссылается.
# Значит, кто-то должен сделать tag-файл wxl, и делает его тот, кому он нужен.

# Куда ведут ссылки на API wxl. По умолчанию — на соседний каталог, который
# `docs_wxl` собирает рядом: так весь сайт работает локально из файловой
# системы, без всякой публикации. Когда сайт wxl где-то опубликуют, сюда
# кладётся его адрес, и Буквица начинает ссылаться наружу.
set(BUKVITSA_WXL_DOCS_URL "../../wxl/html/" CACHE STRING
    "Адрес сайта документации wxl, куда ведут ссылки из документации Буквицы")

# Сайт wxl, собранный рядом. Не копия её собственной цели `docs`, а та же самая
# машинерия с теми же входами: дерево wxl одно, и собирать её сайт из дерева
# Буквицы дешевле, чем требовать двух настроенных деревьев ради одной ссылки.
wxl_add_doxygen_site(docs_wxl
    PROJECT_NAME "wxl"
    BRIEF        "базовая библиотека Буквицы"
    LANGUAGE     Russian
    STRIP_FROM   "${BUKVITSA_WXL_ROOT}"
    MAINPAGE     "${BUKVITSA_WXL_ROOT}/docs/index.md"
    OUTPUT       "${CMAKE_BINARY_DIR}/docs/wxl"
    TAGFILE      "${CMAKE_BINARY_DIR}/docs/wxl.tag"
    # Список повторяет INPUT из wxl/docs/CMakeLists.txt и обязан за ним следовать:
    # новый модуль там, забытый здесь, роняет сборку сайта не отсутствием страниц,
    # а неразрешимой ссылкой на них из index.md -- и только у Буквицы.
    INPUT
        "${BUKVITSA_WXL_ROOT}/docs"
        "${BUKVITSA_WXL_ROOT}/wxl.core/src"
        "${BUKVITSA_WXL_ROOT}/wxl.core/doc"
        "${BUKVITSA_WXL_ROOT}/wxl.core/readme.md"
        "${BUKVITSA_WXL_ROOT}/wxl.text/src"
        "${BUKVITSA_WXL_ROOT}/wxl.text/doc"
        "${BUKVITSA_WXL_ROOT}/wxl.async/src"
        "${BUKVITSA_WXL_ROOT}/wxl.async/doc"
        "${BUKVITSA_WXL_ROOT}/wxl.logging/src"
        "${BUKVITSA_WXL_ROOT}/wxl.logging/doc"
        "${BUKVITSA_WXL_ROOT}/wxl.xml/src"
        "${BUKVITSA_WXL_ROOT}/wxl.winui/src"
        "${BUKVITSA_WXL_ROOT}/wxl.winui/doc"
    EXCLUDE
        "*/CLAUDE.md"
        "*/next-session-prompt.md"
        "*/tests/*"
        "*/test/*"
        "*/bench/*"
        "*/benchmarks/*"
        "*/packaging/*"
        "*/tools/*"
)

# Сайт Буквицы. Обзорные статьи из docs/ и API из заголовков — вместе, потому
# что порознь ни то, ни другое не читается: решения в decisions.md называют
# типы, а типы без причины, по которой они такие, — просто список полей.
wxl_add_doxygen_site(docs
    PROJECT_NAME "Буквица"
    BRIEF        "читалка FB3 под Windows"
    LANGUAGE     Russian
    STRIP_FROM   "${PROJECT_SOURCE_DIR}"
    MAINPAGE     "${PROJECT_SOURCE_DIR}/README.md"
    OUTPUT       "${CMAKE_BINARY_DIR}/docs/bukvitsa"
    TAGS         "${CMAKE_BINARY_DIR}/docs/wxl.tag=${BUKVITSA_WXL_DOCS_URL}"
    DEPENDS      docs_wxl
    INPUT
        "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_SOURCE_DIR}/docs"
        "${PROJECT_SOURCE_DIR}/FB3/src"
        "${PROJECT_SOURCE_DIR}/Typography/include"
        "${PROJECT_SOURCE_DIR}/Typography/src"
        "${PROJECT_SOURCE_DIR}/Reader/src"
    EXCLUDE
        "*/CLAUDE.md"
        "*/tests/*"
        "*/testdata/*"
        "*/demo/*"
        # Бэкенд MicroTeX: определяет члены tex::Font и других чужих классов,
        # объявлений которых Doxygen не видит (external/ в INPUT не входит) и
        # честно считает это ошибкой. Публичная грань формул — formula.h — в
        # сайте остаётся.
        "*/Typography/src/formula.cpp"
)

if(BUKVITSA_BUILD_TESTS)
    wxl_add_doxygen_test(docs)
endif()
