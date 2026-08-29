# Reader — читалка на wxl.winui.
#
# Линкуется с wxl::winui и больше ни с чем из wxl: доступ к декларативной
# поверхности не должен стоить приложению разбора проекции cppwinrt, и
# отсутствие её include-пути здесь — то, что это гарантирует.
#
# WIN32 делает исполняемый файл оконным, а оконным его точкой входа
# становится wWinMain, который живёт в самой wxl.

add_executable(Reader WIN32
    src/main.cpp
    src/start_screen.cpp
    src/book.cpp
    src/book_view.cpp
    src/library_screen.cpp
    src/reader_panel.cpp
    src/note_popup.cpp
    src/book_index.cpp
    src/io.cpp
    src/library.cpp
    src/settings.cpp
    src/store.cpp
    src/file_dialog.cpp
    src/book.h
    src/book_view.h
    src/file_dialog.h
    src/io.h
    src/library.h
    src/library_screen.h
    src/reader_panel.h
    src/note_popup.h
    src/book_index.h
    src/settings.h
    src/start_screen.h
    src/store.h
    src/theme.h
    src/pch.h
    src/app.manifest
)

# bukvitsa::typography тянет за собой bukvitsa::fb3, DirectWrite и Direct2D:
# страницу читалка рисует своими глифами, и рисует их та же вёрстка, что их
# разложила. windowscodecs -- декодирование картинок книги (WIC),
# dxguid -- CLSID встроенных эффектов D2D (изгиб страницы на подложке).
target_link_libraries(Reader PRIVATE
    wxl::winui
    wxl::async
    wxl::xml
    wxl::text
    bukvitsa::typography
    windowscodecs
    dxguid
)

# wxl::xml разбирает settings.xml, а память берёт из STA-пула wxl::core --
# он строится в wxl_launched() и переживает последний разбор.
bukvitsa_consumes_wxl(Reader)

# Путь без схемы разрешается рядом с исполняемым файлом, поэтому ресурсы
# должны там лежать — копируются на каждой сборке, потому что правка картинки
# не повод перелинковывать.
add_custom_command(TARGET Reader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/src/Assets" "$<TARGET_FILE_DIR:Reader>/Assets"
    COMMENT "Копирование ресурсов рядом с Reader")

# Загрузчик Windows App Runtime находит пакет платформы при старте, поэтому
# его DLL обязана лежать рядом. Библиотеку импорта wxl линкует сама — это
# вторая половина её правила «свои зависимости времени выполнения wxl носит
# сама», и приложение по-прежнему не пишет об этом ничего.
set(_reader_bootstrap_dll
    "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll")

if(EXISTS "${_reader_bootstrap_dll}")
    add_custom_command(TARGET Reader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_reader_bootstrap_dll}" "$<TARGET_FILE_DIR:Reader>"
        COMMENT "Копирование загрузчика Windows App Runtime рядом с Reader")
else()
    message(WARNING
        "Нет Microsoft.WindowsAppRuntime.Bootstrap.dll по пути ${_reader_bootstrap_dll}; "
        "Reader не запустится, пока она не окажется рядом с исполняемым файлом.")
endif()

# TODO: реестр обложек и мастер импорта снимка -- план по шагам в
# doc/skin-wizard-plan.txt. Дальше: витрина хранилища, наблюдаемые каталоги,
# второй вариант листания. Всё по порядку -- в docs/reader.md.
