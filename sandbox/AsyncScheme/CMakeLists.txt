# AsyncScheme — черновик асинхронной схемы Буквицы: основной поток с насосом и
# входящей очередью, поток ввода-вывода на wxl::async::threaded_component и
# корутины поверх них. Консольное приложение, а не тест: здесь придумывается
# синтаксис асинхронной операции, и его надо видеть в работе, а не только в
# утверждениях.

add_executable(AsyncScheme
    src/main.cpp
    src/main_loop.cpp
    src/io_thread.cpp
    src/file.cpp
    src/pagination.cpp
    src/async.h
    src/file.h
    src/io_thread.h
    src/main_loop.h
    src/pagination.h
    src/signals.h
)

target_link_libraries(AsyncScheme PRIVATE wxl::async)
bukvitsa_consumes_wxl(AsyncScheme)
