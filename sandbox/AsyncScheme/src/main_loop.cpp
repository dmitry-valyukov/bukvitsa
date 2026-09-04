#include "main_loop.h"

import std;

namespace bukvitsa::io {

namespace {

/// Тот самый единственный насос.
std::atomic<main_loop*> g_main_loop{nullptr};

}  // namespace

void defer(std::function<void()> work)
{
    main_loop* const loop = g_main_loop.load(std::memory_order_acquire);
    if (!loop) throw std::logic_error("Насос основного потока не заведён");

    std::unique_ptr<continuation> item(new continuation(std::move(work)));

    // send() забирает владение и будит читателя, если тот спит. Ни того, ни
    // другого нам знать не надо: канал многописательский, и это его забота.
    loop->inbox_.send(item);
}

main_loop::main_loop()
{
    main_loop* expected = nullptr;
    if (!g_main_loop.compare_exchange_strong(expected, this, std::memory_order_release))
        throw std::logic_error("Насос основного потока уже заведён");
}

main_loop::~main_loop()
{
    g_main_loop.store(nullptr, std::memory_order_release);
}

void main_loop::run_once()
{
    // Сперва окно, потом работа, и порядок этот — весь смысл затеи. Работа
    // приходит нарезанной, и если брать её раньше сообщений, то нарезка ничего
    // не даст: поток будет занят ею подряд, а ввод дождётся конца.
    pump_messages();

    std::unique_ptr<continuation> item;
    if (inbox_.try_receive(item)) {
        item->work();
        return;
    }

    wait_for_work();
}

void main_loop::pump_messages()
{
    MSG msg;

    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

void main_loop::wait_for_work()
{
    // Протокол ожидания ведём сами — так и написано в mpsc_channel про тех, кто
    // ждёт не только его: объявляем, что засыпаем, и с этого мгновения писатель
    // обязан нас будить.
    inbox_.arm();

    // Обязательная перепроверка, а не оптимизация: между try_receive в run_once
    // и объявлением о сне писатель мог положить элемент, застать нас бодрыми и
    // никого не разбудить. Забор связывает его укладку с нашим триггером.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::unique_ptr<continuation> item;
    if (inbox_.try_receive(item)) {
        inbox_.disarm();
        item->work();
        return;
    }

    // Ждём разом две вещи: событие канала и появление сообщений. Событие, а не
    // посланное сообщение: сообщение без окна модальный цикл выбросит вместе с
    // пробуждением, а событие переживает и его.
    HANDLE wait_handle = inbox_.wait_handle();
    ::MsgWaitForMultipleObjects(1, &wait_handle, FALSE, INFINITE, QS_ALLINPUT);

    inbox_.disarm();
}

}  // namespace bukvitsa::io
