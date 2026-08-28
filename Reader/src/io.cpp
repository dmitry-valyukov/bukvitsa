#include <windows.h>

#include <algorithm>

#include "UiThread.h"

// Свой заголовок последним: он импортирует wxl.async (см. io.h).
#include "io.h"

namespace bukvitsa::reader {

using wxl::async::awaitable;
using wxl::async::task;
using wxl::core::directory;
using wxl::core::file;
using wxl::core::path;

namespace {

/// Читает файл целиком. На рабочем потоке.
std::optional<std::string> readWhole(const path& p) {
    file source = file::open_read(p.c_str());

    if (!source.opened()) return std::nullopt;

    const std::optional<std::uint64_t> length = source.size();

    if (!length) return std::nullopt;

    std::string bytes(static_cast<std::size_t>(*length), '\0');

    const std::size_t read =
        source.read({reinterpret_cast<std::byte*>(bytes.data()), bytes.size()});

    bytes.resize(read);

    return bytes;
}

/// Пишет файл целиком: временный рядом, на диск, потом заменой. На рабочем
/// потоке.
///
/// Все три пути -- сам файл, его каталог и временный -- приходят готовыми, и
/// не для удобства: путь держит символы в STA-пуле, а пул принадлежит
/// интерфейсному потоку, так что собрать здесь хотя бы один было бы обращением
/// в чужой пул.
///
/// Сброс на носитель до переименования -- не педантизм: без него выключение
/// питания могло бы оставить переименование сделанным, а содержимое -- нет, и
/// на месте реестра оказался бы файл с правильным именем и нулями внутри.
bool writeWhole(const path& p, path& parent, const path& temporary, std::string_view content) {
    if (!parent.empty() && !directory::create_all(parent)) return false;

    {
        file out = file::create(temporary.c_str());

        if (!out.opened()) return false;

        const std::size_t written =
            out.write({reinterpret_cast<const std::byte*>(content.data()), content.size()});

        if (written != content.size() || !out.flush()) return false;
    }

    // WRITE_THROUGH -- чтобы и сама замена дошла до диска, а не осталась в
    // кэше, если следом выключат питание.
    return ::MoveFileExW(temporary.c_str(), p.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace

path poolPath(const std::filesystem::path& path) {
    return wxl::core::path(std::wstring_view(path.native()));
}

Io::Io() : loop_("Буквица: ввод-вывод") {}

Io::~Io() {
    stop();
}

void Io::start(const wxl::DispatcherQueue& queue) {
    // Очередь берётся здесь, в интерфейсном потоке, и живёт в замыкании,
    // которое зовёт рабочий: `wxl::UiThread` для того и есть -- обёртки wxl
    // однопоточны, а очередь под ними agile.
    const wxl::UiThread ui{queue};

    loop_.wake_with([this, ui]() noexcept {
        // Рабочий поток: всё, что тут можно, -- попросить интерфейсный
        // разобрать вернувшееся. Не получилось (очередь закрывается) --
        // значит приложение уходит, и разбирать уже некому.
        ui.post([this] {
            loop_.run_pending();
            collect();
        });
    });

    loop_.start();

    started_ = true;
}

void Io::stop() {
    if (!started_) return;

    started_ = false;
    loop_.stop();

    // Корутины, чьи операции не успели вернуться, так и остались
    // приостановленными: возобновлять их некому и незачем. Их кадры уходят
    // вместе с задачами.
    running_.clear();
}

void Io::spawn(task&& work) {
    // Кончилась, не дойдя до первого co_await, -- держать нечего.
    if (work.done()) {
        work.result();
        return;
    }

    running_.push_back(std::move(work));
}

void Io::collect() {
    // Исключение из корутины -- это исключение приложения, а не рабочего
    // потока: result() бросает его здесь, в интерфейсном потоке, где ему и
    // место.
    for (task& work : running_) {
        if (work.done()) work.result();
    }

    std::erase_if(running_, [](const task& work) { return work.done(); });
}

awaitable<std::optional<std::string>> Io::readFile(const std::filesystem::path& file_path) {
    return loop_.async_call([p = poolPath(file_path)] { return readWhole(p); });
}

awaitable<bool> Io::writeFile(const std::filesystem::path& file_path, std::string content) {
    std::filesystem::path temporary = file_path;
    temporary += L".tmp";

    return loop_.async_call([p = poolPath(file_path), parent = poolPath(file_path.parent_path()),
                             tmp = poolPath(temporary),
                             text = std::move(content)]() mutable {
        return writeWhole(p, parent, tmp, text);
    });
}

awaitable<bool> Io::fileExists(const std::filesystem::path& file_path) {
    return loop_.async_call([p = poolPath(file_path)] {
        const DWORD attributes = ::GetFileAttributesW(p.c_str());

        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    });
}

awaitable<std::uint64_t> Io::fileSize(const std::filesystem::path& file_path) {
    return loop_.async_call([p = poolPath(file_path)]() -> std::uint64_t {
        file source = file::open_read(p.c_str());

        return source.size().value_or(0);
    });
}

awaitable<std::vector<DirectoryEntry>> Io::list(const std::filesystem::path& directory_path,
                                                std::wstring_view mask) {
    return loop_.async_call(
        [pattern = poolPath(directory_path) / mask]() -> std::vector<DirectoryEntry> {
            std::vector<DirectoryEntry> found;

            directory listing = directory::open(pattern.c_str());

            for (directory::entry entry; listing.next(entry);) {
                found.push_back(DirectoryEntry{std::wstring(entry.name), entry.is_directory,
                                               entry.size});
            }

            return found;
        });
}

}  // namespace bukvitsa::reader
