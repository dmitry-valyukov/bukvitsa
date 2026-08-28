#pragma once

#include "async.h"
#include "io_thread.h"

namespace bukvitsa::io {

/// Буфер чтения.
///
/// Пока просто вектор. Своим он станет ради `resize()`, который не
/// инициализирует байты: читать в буфер, который перед этим занулили, — двойная
/// работа, и на потоке файла она видна.
///
/// Ёмкость буфера — это и есть размер порции чтения: `read()` дочитывает его до
/// `capacity()`, а `clear()` возвращает место, не отдавая памяти.
using buffer = std::vector<char>;

class read_operation;

/// Половина ожидания, общая всем операциям: `co_await` отправляет работу в
/// поток ввода-вывода, а возобновление приезжает обратно.
///
/// Все три исхода — значение, ошибка, отмена — возвращаются одним и тем же
/// путём и достаются вызывающему в том потоке, где он операцию создал. Ошибку
/// операция ловит в чужом потоке и хранит до возобновления; передача её сюда
/// упорядочена самой очередью, поэтому синхронизации у поля нет и не нужно.
class operation_base
{
public:
    /// Готовых ответов у этой схемы не бывает: даже отменённая операция едет
    /// в поток ввода-вывода и обратно, чтобы исход всегда приходил одинаково.
    bool await_ready() const noexcept { return false; }

protected:
    explicit operation_base(cancellation_token token) noexcept : token_(std::move(token)) {}

    /// \param work то, что исполнится в потоке ввода-вывода
    void start(std::coroutine_handle<> continuation, std::function<void()> work);

    void rethrow_if_failed() const {
        if (error_) std::rethrow_exception(error_);
    }

private:
    cancellation_token token_;
    std::exception_ptr error_;
};

/// Открытый файл: читается в потоке ввода-вывода, а живёт в том, где его
/// открыли.
///
/// Владение: `reader` владеет описателем файла и закрывает его сам. Копировать
/// его нельзя, перемещать можно.
class reader
{
public:
    reader() noexcept = default;

    reader(reader&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)), eof_(other.eof_) {}

    reader& operator=(reader&& other) noexcept {
        std::swap(handle_, other.handle_);
        std::swap(eof_, other.eof_);
        return *this;
    }

    ~reader();

    /// Конец файла — не «дошли до последнего байта», а «чтение вернуло ноль»:
    /// узнать это можно только по факту, поэтому последняя итерация цикла
    /// чтения всегда вхолостую.
    bool is_eof() const noexcept { return eof_; }

    /// Дочитывает буфер до его ёмкости.
    ///
    /// \param into буфер; прочитанное дописывается к тому, что в нём уже есть,
    ///        поэтому места в нём должно быть хотя бы на байт.
    /// \param token право отменить: проверяется перед чтением, уже начатое не
    ///        прерывает.
    [[nodiscard]] read_operation read(buffer& into, cancellation_token token = {});

private:
    friend class open_operation;
    friend class read_operation;

    explicit reader(void* handle) noexcept : handle_(handle) {}

    /// Настоящее чтение, в потоке ввода-вывода.
    void read_blocking(buffer& into);

    void* handle_ = nullptr;

    /// Пишется в потоке ввода-вывода, читается в основном; передача
    /// упорядочена очередью, через которую приезжает возобновление.
    bool eof_ = false;
};

/// Ожидание чтения. Живёт в кадре корутины, пока `co_await` не кончится.
class read_operation : public operation_base
{
public:
    read_operation(reader& source, buffer& into, cancellation_token token) noexcept
        : operation_base(std::move(token)), reader_(&source), buffer_(&into) {}

    void await_suspend(std::coroutine_handle<> continuation) {
        start(continuation, [this] { reader_->read_blocking(*buffer_); });
    }

    /// \throw то, чем чтение кончилось, если оно кончилось не данными.
    void await_resume() const { rethrow_if_failed(); }

private:
    reader* const reader_;
    buffer* const buffer_;
};

/// Ожидание открытия — оно же то, что отдаёт `reader`.
class open_operation : public operation_base
{
public:
    open_operation(std::filesystem::path path, cancellation_token token)
        : operation_base(std::move(token)), path_(std::move(path)) {}

    void await_suspend(std::coroutine_handle<> continuation) {
        start(continuation, [this] { open_blocking(); });
    }

    /// \return открытый файл.
    /// \throw std::system_error если открыть не удалось.
    reader await_resume() {
        rethrow_if_failed();
        return reader(std::exchange(handle_, nullptr));
    }

private:
    /// Настоящее открытие, в потоке ввода-вывода.
    void open_blocking();

    std::filesystem::path path_;
    void* handle_ = nullptr;
};

namespace file {

/// Открывает файл на чтение.
///
/// \param token право отменить: проверяется перед открытием.
[[nodiscard]] inline open_operation open_async(std::filesystem::path path,
                                               cancellation_token token = {}) {
    return open_operation(std::move(path), std::move(token));
}

}  // namespace file

}  // namespace bukvitsa::io
