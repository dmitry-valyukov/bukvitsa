#pragma once

// Импорт после заголовков — правило проекта.
import wxl.core;

namespace bukvitsa::io {

/// Пробуждение потока ввода-вывода: он спит на семафоре и больше ни на чём.
///
/// У основного потока пробуждение своё и здесь не описано: он спит не на
/// примитиве очереди, а сразу на двух вещах — событии своей очереди и
/// сообщениях окна, — и делает это сам, см. `main_loop::wait_for_work()`.
class semaphore_signal
{
public:
    void set() { semaphore_.release(); }
    void wait() { semaphore_.acquire(); }

private:
    wxl::core::semaphore semaphore_{0};
};

}  // namespace bukvitsa::io
