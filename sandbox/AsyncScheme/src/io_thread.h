#pragma once

#include <windows.h>

#include "async.h"
#include "signals.h"

import wxl.async;

namespace bukvitsa::io {

/// Работа, отданная в поток ввода-вывода.
///
/// Обе половины лежат в одном элементе: то, что исполнится там, и то, что после
/// этого исполнится в основном потоке.
struct job {
    std::function<void()> work;      ///< исполняется в потоке ввода-вывода
    std::function<void()> when_done; ///< исполняется в основном потоке
};

/// Ставит задачу в поток ввода-вывода. Только из основного потока: у очереди
/// туда ровно один писатель, и это он.
///
/// \throw std::logic_error если поток ввода-вывода не запущен.
void post(job next);

/// Поток ввода-вывода: компонент wxl со своим потоком и очередь к нему.
///
/// Очередь тут одна, и она односторонняя: кладёт в неё только основной поток,
/// поэтому SPSC ей по мерке, а инвариант охраняется assert'ом в post().
/// Обратной очереди у потока нет — исполнив работу, он отдаёт продолжение
/// общим `defer()`, во входящую очередь основного потока. Она многописательская
/// нарочно: сегодня в неё пишет этот поток и сам основной, а завтра — второй
/// рабочий, если он появится.
///
/// Экземпляр один на процесс: он объявляет себя тем самым потоком, в который
/// смотрит post(), — построить второй нельзя. Владеет им тот, кто его создал,
/// то есть main().
class io_thread : public wxl::async::threaded_component
{
    using base = wxl::async::threaded_component;

public:
    static constexpr std::size_t block_size = 64;

    using to_worker = spsc_channel<job, block_size, semaphore_signal>;

    /// Строится в основном потоке: отсюда берётся тот единственный поток,
    /// которому позволено звать post().
    explicit io_thread(wxl::async::thread_group* group);
    ~io_thread() override;

protected:
    void run() override;

    void on_stopping() override;

private:
    friend void post(job next);

    const DWORD main_thread_{::GetCurrentThreadId()};

    to_worker to_worker_;
};

}  // namespace bukvitsa::io
