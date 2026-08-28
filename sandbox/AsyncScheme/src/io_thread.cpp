#include "io_thread.h"

#include "main_loop.h"

import std;

namespace bukvitsa::io {

namespace {

/// Тот самый единственный поток. Атомарный, потому что читается из чужих
/// потоков, а меняется только в конструкторе и деструкторе.
std::atomic<io_thread*> g_io_thread{nullptr};

}  // namespace

void post(job next)
{
    io_thread* const io = g_io_thread.load(std::memory_order_acquire);
    if (!io) throw std::logic_error("Поток ввода-вывода не запущен");

    // Писатель в эту очередь один — основной поток. Без этой проверки чужой
    // вызов сломал бы очередь молча, а не заметно.
    assert(::GetCurrentThreadId() == io->main_thread_ &&
           "post() — только из основного потока");

    io->to_worker_.send(std::move(next));
}

io_thread::io_thread(wxl::async::thread_group* group)
    : base("Bukvitsa IO", group)
{
    io_thread* expected = nullptr;
    if (!g_io_thread.compare_exchange_strong(expected, this, std::memory_order_release))
        throw std::logic_error("Поток ввода-вывода уже создан");
}

io_thread::~io_thread()
{
    // Обязателен в самом производном типе: держит объект, пока компонент не
    // остановился совсем, иначе задача в полёте достанется полуразрушенному.
    dispose();

    g_io_thread.store(nullptr, std::memory_order_release);
}

void io_thread::run()
{
    // Читатель заводится здесь, а не в конструкторе: он принадлежит читающему
    // потоку и обязан умереть раньше очереди — выход из run() это и делает.
    to_worker::reader jobs(to_worker_);

    const auto execute = [](job& next) {
        next.work();

        // Обратный ход. Продолжение исполняет не тот, кто его создал, и не тот,
        // кто закончил работу, — основной поток, и только он. Своей очереди для
        // этого у нас нет: у основного потока одна входящая на всех.
        defer(std::move(next.when_done));
    };

    // Работа. Ложное пробуждение протоколом разрешено, поэтому условие цикла
    // своё — им и оказывается просьба остановиться.
    while (!stop_requested()) {
        job next;
        if (jobs.receive(next)) execute(next);
    }

    // Завершение: доиграть то, что уже положено.
    //
    // Отдельная фаза, а не выход по флагу, потому что флаг поднимается раньше
    // закрытия канала — component зовёт on_stopping() уже при stop_requested(),
    // и между этими двумя моментами положить ещё успевают. Поэтому уходим не по
    // флагу, а по закрытому и пустому каналу: защёлкнутый турникет и есть
    // обещание, что новых задач не будет.
    for (;;) {
        job next;

        // Сперва вычерпать, не засыпая, и только потом решать, уходить ли:
        // проверка закрытия обязана стоять ПЕРЕД сном, иначе поток, разбуженный
        // закрытием ещё в рабочей фазе, уснёт здесь навсегда — второго
        // signal(true) не будет.
        if (jobs.read(next)) {
            execute(next);
            continue;
        }

        if (to_worker_.closed()) break;

        // Канал ещё открыт: закрытие в пути, а положить успевают и сейчас.
        // Спать можно — разбудит либо новая задача, либо само закрытие.
        if (jobs.receive(next)) execute(next);
    }
}

void io_thread::on_stopping()
{
    base::on_stopping();

    // Закрыть, а потом будить, и не наоборот: проснувшийся раньше закрытия
    // увидит канал открытым и уснёт снова — а второго сигнала уже не будет.
    to_worker_.close();
    to_worker_.signal(true);
}

}  // namespace bukvitsa::io
