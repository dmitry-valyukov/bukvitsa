cmake_minimum_required(VERSION 3.28)

# ---- wxl: базовая библиотека Буквицы, соседнее дерево ----
#
# Задаётся здесь, а не в FB3/, по одной причине: wxl собирается с `import std;`,
# а экспериментальный шлюз CMake для него открывается только в самом верхнем
# CMakeLists и только ДО project(). Скрипт шлюза живёт в wxl и знает UUID,
# привязанный к версии CMake, — включаем его оттуда, чтобы не заводить второе
# место, где этот UUID придётся обновлять.
set(BUKVITSA_WXL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../wxl" CACHE PATH
    "Корень дерева wxl (даёт wxl::core и wxl::xml)")

if(NOT EXISTS "${BUKVITSA_WXL_ROOT}/cmake/wxl_import_std_gate.cmake")
    message(FATAL_ERROR
        "Не найден wxl: ожидался в ${BUKVITSA_WXL_ROOT}. "
        "Задайте -DBUKVITSA_WXL_ROOT=<путь к дереву wxl>.")
endif()

set(BUKVITSA_WXL_CMAKE_DIR "${BUKVITSA_WXL_ROOT}/cmake")

# vcpkg — ради cppwinrt, из которого собрана проекция WinUI 3 под wxl.winui,
# и ради fmt, которым форматирует wxl.text. Тоже до project(), потому что
# ставит CMAKE_TOOLCHAIN_FILE, а его после первого project() уже никто не
# читает. Манифеста у Буквицы нет: из всего пакета берутся эти два, и оба —
# по требованию wxl, а не её собственному.
include("${BUKVITSA_WXL_CMAKE_DIR}/vcpkg_bootstrap.cmake")

include("${BUKVITSA_WXL_CMAKE_DIR}/wxl_import_std_gate.cmake")

project(Bukvitsa
    VERSION 0.1.0
    DESCRIPTION "Bukvitsa: home reading ecosystem (FB3 + Typography + Reader)"
    LANGUAGES CXX)

# C++23, хотя сама Буквица обходится двадцатым: модуль std CMake собирает по
# стандарту всего проекта, а wxl требует от своих потребителей двадцать третьего.
# Разойдясь, они дают C5050 «mismatched C++ versions» на каждый импорт и падение
# на компоновке — то есть далеко от места, где стоит настоящая причина.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Оптимизация Release ----
#
# Задаётся здесь, до add_subdirectory(wxl...), чтобы одинаково накрыть и wxl:
# смешивать в одной программе объекты с /GL и без него можно, но тогда LTO
# видит только половину картины, а именно в wxl.xml лежит разбор, который
# Буквица зовёт чаще всего.
#
# /O2 уже включает /Oi /Ot /Gy /GF /Oy — их не дублируем. Добавляем то, чего
# в нём нет:
#   /Ob3 — встраивать агрессивнее, чем /Ob2. Вёрстка — это внутренние циклы
#          из мелких функций (характеристики символа, ширина пробела), и
#          выигрывают именно они.
#   /Gw  — раскладывать глобальные данные по COMDAT, как /Gy делает с
#          функциями. Без него компоновщик не может выбросить неиспользуемую
#          таблицу, потому что не видит её отдельно от соседней.
#   /GS- — снять проверку переполнения буферов. Она стоит пролога и эпилога
#          в каждой функции с массивом на стеке, а такие функции — весь
#          разбор. Решение: за корректностью следит сам разбор, а не
#          страховка компилятора; отсюда требование к разбору — не писать
#          в буфер, не проверив длину, и не считать длину из файла верной.
if(MSVC)
    # /Ob3 именно заменой, а не добавкой: CMake кладёт в свои флаги Release
    # /Ob2, а два ключа встраивания в одной строке дают D9025 на каждом файле —
    # предупреждение верное по сути (один ключ молча отменяет другой) и потому
    # заслуживает не глушения, а исправления.
    string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    add_compile_options($<$<CONFIG:Release>:/Gw> $<$<CONFIG:Release>:/GS->)

    # /OPT:REF выбрасывает недостижимые функции и данные, /OPT:ICF сливает
    # одинаковые. Компоновщик включает их сам, когда сборка не инкрементальная,
    # но полагаться на это неудобно: /INCREMENTAL приходит из флагов CMake и
    # может измениться, а тогда удаление тихо отключится.
    add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# LTO (у MSVC — /GL при компиляции и /LTCG при компоновке). Через свойство
# CMake, а не флагом руками: /GL нужен и статическим библиотекам, и их
# библиотекарю, и исполняемому файлу, и порядок этих флагов CMake знает лучше.
#
# Без выключателя: править и смотреть, что вышло, полагается в Debug, а Release
# собирается редко — лишние секунды компоновки некому мешать.
include(CheckIPOSupported)
check_ipo_supported(RESULT BUKVITSA_IPO_SUPPORTED OUTPUT BUKVITSA_IPO_ERROR)
if(BUKVITSA_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(WARNING "LTO недоступен, Release собирается без него: ${BUKVITSA_IPO_ERROR}")
endif()

# Ключи, задающие язык, — до подключения wxl и для всего дерева сразу.
#
# /utf-8 определяет _UTF8, а MSVC сверяет макросы при импорте модуля и ругается
# C5050 на каждый импорт, если BMI собран с другим набором.
#
# /permissive- и /Zc:__cplusplus здесь по причине потяжелее. CMake считает BMI
# годным только там, где язык настроен так же: цель с другим набором получает
# не общий BMI, а свой — CMake заводит синтетическую цель `<цель>@synth_N` и
# пересобирает в ней интерфейсы wxl.text, wxl.core и wxl.xml. Пока модули были
# только у wxl, это стоило лишь времени сборки; с тех пор как модулем стал FB3,
# обе копии встречаются в одной цели, и сборка падает с «Disagreement of the
# location of the module». Одинаковые ключи снимают и то, и другое: копия
# ровно одна.
#
# /W4 сюда не входит намеренно — и потому, что уровень предупреждений на
# годность BMI не влияет (проверено: синтетических целей от него не заводится),
# и потому, что предупреждения wxl — её дело, а не шум в сборке читалки. Он
# стоит ниже, вместе с определениями, и накрывает только код Буквицы.
#
# wxl собирает себя с /permissive- и /utf-8 (`cmake/config.cmake`), так что это
# не навязывание чужому проекту, а совпадение с его собственной настройкой.
if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus)
endif()

# ---- Подключение wxl ----
#
# Берём два подкаталога с исходниками, а не корневые CMakeLists модулей: те
# устроены как самостоятельные проекты и тянут vcpkg, Boost, GTest, cppwinrt и
# FetchContent winmd — всё это нужно генератору wxl.winui и не нужно читалке.
# wxl.core/src и wxl.xml/src обходятся своими скриптами-помощниками.
#
# EXCLUDE_FROM_ALL: собирается только то, что действительно связывается с
# Буквицей.
include("${BUKVITSA_WXL_CMAKE_DIR}/wxl_helpers.cmake")

if(NOT TARGET wxl::core)
    add_subdirectory("${BUKVITSA_WXL_ROOT}/wxl.core/src" "${CMAKE_BINARY_DIR}/wxl.core" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET wxl::xml)
    add_subdirectory("${BUKVITSA_WXL_ROOT}/wxl.xml/src" "${CMAKE_BINARY_DIR}/wxl.xml" EXCLUDE_FROM_ALL)
endif()

# wxl.text — перекодировка, разбор чисел, сборка строк. Всё, что Буквица
# раньше держала у себя тремя копиями: UTF-8 ⇄ UTF-16 в FB3 и в читалке,
# разбор кодовых точек в вёрстке, экранирование XML при записи файлов. Её же
# зовут wxl.xml, wxl.async и wxl.winui — дерево wxl работает с текстом через
# неё, а не мимо, — так что без неё не настроится и то, что Буквица линкует
# помимо своего кода.
if(NOT TARGET wxl::text)
    add_subdirectory("${BUKVITSA_WXL_ROOT}/wxl.text/src" "${CMAKE_BINARY_DIR}/wxl.text" EXCLUDE_FROM_ALL)
endif()

# wxl.async — ради черновика асинхронной схемы в sandbox/. Читалка его пока не
# линкует, поэтому EXCLUDE_FROM_ALL здесь не формальность: без цели, которая
# его назвала, он и не собирается.
if(NOT TARGET wxl::async)
    add_subdirectory("${BUKVITSA_WXL_ROOT}/wxl.async/src" "${CMAKE_BINARY_DIR}/wxl.async" EXCLUDE_FROM_ALL)
endif()

# Семейство разметок — их требует wxl.winui: контролы HtmlBlock, BbBlock и
# RsdnBlock линкуют свои парсеры. Той же формой, что модули выше.
if(NOT TARGET wxl::html)
    add_subdirectory("${BUKVITSA_WXL_ROOT}/wxl.html/src" "${CMAKE_BINARY_DIR}/wxl.html" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET wxl::bb)
    add_subdirectory("${BUKVITSA_WXL_ROOT}/wxl.bb/src" "${CMAKE_BINARY_DIR}/wxl.bb" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET wxl::rsdn)
    add_subdirectory("${BUKVITSA_WXL_ROOT}/wxl.rsdn/src" "${CMAKE_BINARY_DIR}/wxl.rsdn" EXCLUDE_FROM_ALL)
endif()

# wxl.winui — проекция WinUI 3, на которой стоит Reader. Здесь, в отличие от
# тех, что выше, берётся корневой CMakeLists модуля: он и есть весь модуль, а
# генератор с его winmd и FetchContent живёт отдельной целью в дереве wxl и
# сюда не приезжает. Собирается из профиля rich, уже выпущенного в
# wxl.winui/src/generated — сборка генератор не запускает, это отдельный шаг.
if(WIN32 AND NOT TARGET wxl::winui)
    add_subdirectory("${BUKVITSA_WXL_ROOT}/wxl.winui" "${CMAKE_BINARY_DIR}/wxl.winui" EXCLUDE_FROM_ALL)
endif()

# Цель, чьи исходники видят заголовки wxl, компилирует их `import std;` сама:
# без этих двух свойств MSVC не найдёт модуль std, а CMake не станет сканировать
# обычные .cpp на импорты (CMP0155 при cmake_minimum_required < 3.28).
function(bukvitsa_consumes_wxl target)
    set_target_properties(${target} PROPERTIES
        CXX_MODULE_STD ON
        CXX_SCAN_FOR_MODULES ON
    )
endfunction()

# Вендоренные зависимости — до /W4 и определений ниже по той же причине, по
# которой их не получает wxl: чужой код с чужими предупреждениями. Сейчас тут
# MicroTeX (движок формул) с tinyxml2 внутри, см. external/README.md.
add_subdirectory(external)

# Своё Буквицы — строго после подключения wxl, в отличие от ключей языка выше.
# Определения: у wxl свои NOMINMAX и WIN32_LEAN_AND_MEAN в platform.h, и наши
# дали бы C4005 на каждом её файле. /W4: предупреждения wxl — её дело, и в
# сборке читалки им место только шумом.
if(MSVC)
    add_compile_options(/W4)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(BUKVITSA_BUILD_TESTS "Build unit tests" ON)
if(BUKVITSA_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(FB3)
add_subdirectory(MathML)
add_subdirectory(Typography)
if(WIN32)
    add_subdirectory(Reader)
endif()

# Черновики. В сборке — потому что черновик, который не собирается, ничего не
# проверяет: отдельной цели, которую надо назвать явно, для этого хватает.
add_subdirectory(sandbox/AsyncScheme)

# ---- Документация ----
#
# Машинерия берётся из дерева wxl — тот же источник, что у vcpkg_bootstrap и
# wxl_helpers. Выключателем, а не всегда: настройка один раз на дерево сборки
# лезет в сеть (за темой, а при отсутствии свежего Doxygen — и за ним самим),
# и дереву, настроенному ради сборки читалки, ждать этого незачем.
option(BUKVITSA_BUILD_DOCS "Собирать сайт документации (Doxygen)" ON)
if(BUKVITSA_BUILD_DOCS)
    include("${BUKVITSA_WXL_CMAKE_DIR}/wxl_doxygen.cmake")
    add_subdirectory(docs)
endif()
