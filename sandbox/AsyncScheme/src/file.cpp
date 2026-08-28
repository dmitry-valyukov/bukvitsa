#include "file.h"

import std;

namespace bukvitsa::io {

namespace {

/// Ошибка Windows как исключение: `GetLastError()` спрашивается сразу, пока
/// его не затёр следующий вызов.
[[noreturn]] void throw_last_error(const char* what)
{
    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), what);
}

}  // namespace

void operation_base::start(std::coroutine_handle<> continuation, std::function<void()> work)
{
    post({
        // Исполняется в потоке ввода-вывода. Отмена проверяется здесь, а не на
        // месте вызова: пока задача лежала в очереди, отменить успели, и это
        // самый частый случай отмены вообще.
        .work =
            [this, work = std::move(work)] {
                try {
                    token_.throw_if_canceled();
                    work();
                } catch (...) {
                    error_ = std::current_exception();
                }
            },

        // Исполняется в основном потоке — том, где корутина писалась и где она
        // продолжится. Возобновление здесь и есть весь возврат: значение,
        // ошибка и отмена уже лежат в операции, а операция живёт в кадре.
        .when_done = [continuation] { continuation.resume(); },
    });
}

reader::~reader()
{
    // Закрываем сами, здесь же: единственный файловый вызов схемы, сделанный не
    // в потоке ввода-вывода, и так решено — по двум причинам.
    //
    // Дешевизна: CloseHandle не ходит на диск, он отдаёт описатель, поэтому
    // гонять ради него задачу туда и обратно дороже самого вызова, а
    // деструктору пришлось бы ещё и дожидаться ответа.
    //
    // И главное — надёжность на закрытии программы. К концу работы поток
    // ввода-вывода защёлкивает свой турникет, и отправить туда что-либо уже
    // нельзя: reader, доживший до этого момента, не смог бы закрыть свой файл
    // вовсе. Деструктор не имеет права зависеть от канала, который к тому
    // времени может быть закрыт.
    if (handle_) ::CloseHandle(handle_);
}

read_operation reader::read(buffer& into, cancellation_token token)
{
    return read_operation(*this, into, std::move(token));
}

void reader::read_blocking(buffer& into)
{
    const std::size_t filled = into.size();
    const std::size_t room = into.capacity() - filled;

    if (room == 0) throw std::logic_error("io::reader::read: в буфере нет места");

    // Вот та самая двойная работа, ради которой буфер когда-то станет своим:
    // vector обязан занулить байты, в которые сейчас же придёт файл.
    into.resize(into.capacity());

    DWORD taken = 0;
    if (!::ReadFile(handle_, into.data() + filled, static_cast<DWORD>(room), &taken, nullptr)) {
        into.resize(filled);
        throw_last_error("ReadFile");
    }

    into.resize(filled + taken);

    // Ноль байт без ошибки означает конец файла — узнать это заранее нельзя.
    if (taken == 0) eof_ = true;
}

void open_operation::open_blocking()
{
    HANDLE handle = ::CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (handle == INVALID_HANDLE_VALUE) throw_last_error("CreateFile");

    handle_ = handle;
}

}  // namespace bukvitsa::io
