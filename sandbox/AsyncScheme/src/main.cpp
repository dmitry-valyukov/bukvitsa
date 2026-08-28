// Черновик асинхронной схемы Буквицы.
//
// Основной поток здесь играет STA-поток приложения: он не блокируется в
// ожидании, а спит сразу на двух вещах — своей входящей очереди и сообщениях
// окна. Кладут в эту очередь все: поток ввода-вывода — продолжения, сам
// основной поток — то, что отложил себе, и всякий, кто появится позже. Поверх
// этого — три сценария: чтение файла корутиной, десяток таких корутин разом и
// пагинация, которая не уезжает никуда, но и поток не занимает.

#include "file.h"
#include "main_loop.h"
#include "pagination.h"

import std;

namespace io = bukvitsa::io;

/// Чтение файла, записанное так, как будто оно синхронное. Каждое co_await
/// уезжает в поток ввода-вывода и возвращается сюда — код между ними
/// исполняется в основном потоке и больше нигде.
io::task print_file(std::filesystem::path path)
{
    io::reader rd = co_await io::file::open_async(path);

    io::buffer buffer(64);

    while (!rd.is_eof()) {
        buffer.clear();
        co_await rd.read(buffer);
        std::cout << std::string_view(buffer.data(), buffer.size());
    }
}

/// Отчёт одного из десяти читателей.
struct reading_report {
    std::size_t bytes = 0;  ///< сколько прочитано всего
    int chunks = 0;         ///< за сколько кусков
};

/// Тот же цикл, но вместо содержимого — счёт и метка, и с мелким буфером, чтобы
/// кусков было побольше, а переключений между корутинами — тем более.
///
/// Своего мира корутины не покидают: у каждой свой описатель файла, своя
/// позиция в нём, свой буфер и свой кадр, а про соседок она не знает ничего.
/// Общая у них только очередь, и та не их забота. Отсюда и то, что `report`
/// ничем не защищён: он принадлежит ей одной, а код между `co_await`
/// исполняется только в основном потоке.
io::task read_counting(std::filesystem::path path, reading_report& report, char mark)
{
    io::reader rd = co_await io::file::open_async(path);

    io::buffer buffer(16);

    while (!rd.is_eof()) {
        buffer.clear();
        co_await rd.read(buffer);

        if (buffer.empty()) break;

        report.bytes += buffer.size();
        ++report.chunks;

        // Лента, по которой снаружи видно чередование. Корутина просто
        // отмечается — читать её она не читает и о соседках не догадывается.
        std::cout << mark;
    }
}

/// То же начало, но с правом отмены: здесь оно понадобится сразу.
io::task open_with(std::filesystem::path path, io::cancellation_token token)
{
    [[maybe_unused]] io::reader rd = co_await io::file::open_async(path, token);
}

/// «Окно»: пока идёт пагинация, поток обязан оставаться свободным.
///
/// Каждая точка на ленте — оборот насоса, на котором поток занялся не
/// пагинацией. В настоящем приложении на этом месте разбирается ввод и
/// перерисовывается окно; здесь достаточно того, что обороты вообще есть.
io::task ui_heartbeat(const bool& paginated)
{
    while (!paginated) {
        co_await io::yield();
        std::cout << '.';
    }
}

namespace {

/// Файл для показа, если своего не дали: пишется рядом с временными.
std::filesystem::path write_sample()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "AsyncScheme.txt";

    std::ofstream(path, std::ios::binary) << "Буквица читает файл по кусочкам в 64 байта,\n"
                                          << "и каждый кусочек приезжает из чужого потока.\n"
                                          << "Между co_await мы снова в основном потоке.\n";
    return path;
}

}  // namespace

int main(int argc, char** argv)
{
    ::SetConsoleOutputCP(CP_UTF8);

    // thread_scope владеет группой потоков и не отпускает main(), пока они не
    // завершились: компонент отдаёт ей свой поток при старте.
    wxl::async::thread_scope threads;

    // Насос — раньше потока ввода-вывода и переживает его: останавливаясь, тот
    // доигрывает оставшиеся задачи и отдаёт их продолжения сюда, во входящую
    // очередь. Насос, умерший первым, оставил бы их без адресата.
    io::main_loop loop;

    io::io_thread worker(threads);
    worker.start_async().get();

    const std::filesystem::path path = argc > 1 ? std::filesystem::path(argv[1]) : write_sample();

    std::println("--- одна корутина ---");

    io::task reading = print_file(path);
    loop.run_until([&reading] { return reading.done(); });
    reading.result();  // бросит то, чем кончилось чтение, если оно упало

    // Десяток читателей одного и того же файла. Каждый со своим описателем,
    // своей позицией и своим кадром; общая у них только очередь.
    std::println("--- десять корутин ---");

    constexpr int readers = 10;

    std::vector<reading_report> reports(readers);
    std::vector<io::task> readings;
    readings.reserve(readers);

    std::cout << "порядок кусков: ";

    for (int i = 0; i < readers; ++i)
        readings.emplace_back(read_counting(path, reports[i], static_cast<char>('0' + i)));

    loop.run_until([&readings] { return std::ranges::all_of(readings, &io::task::done); });

    for (io::task& reading_task : readings) reading_task.result();

    const std::size_t expected = std::filesystem::file_size(path);
    const bool all_read_whole = std::ranges::all_of(
        reports, [expected](const reading_report& r) { return r.bytes == expected; });

    std::println("");
    std::println("кусков у каждой по {}", reports.front().chunks);
    std::println("прочитано по {} байт каждой: {}", expected, all_read_whole ? "да" : "НЕТ");

    // Отмена: право отзывается до того, как операция доедет до потока
    // ввода-вывода, и он за неё даже не берётся.
    std::println("--- отмена ---");

    io::cancellation_source canceling;
    canceling.cancel();

    io::task canceled = open_with(path, canceling.token());
    loop.run_until([&canceled] { return canceled.done(); });

    try {
        canceled.result();
        std::println("отмена не сработала");
    } catch (const std::exception& error) {
        std::println("отмена: {}", error.what());
    }

    // Пагинация: асинхронная работа, которая никуда не уезжает. Здесь важно не
    // то, что она когда-нибудь досчитает, а то, что между её кусками поток
    // достаётся окну — на ленте это точки вперемежку с решётками.
    std::println("--- пагинация в потоке GUI ---");

    bukvitsa::Layout layout;
    bukvitsa::Pages pages;
    bool paginated = false;

    io::task ui = ui_heartbeat(paginated);

    constexpr int layoutChanges = 2;

    for (int attempt = 0; attempt <= layoutChanges; ++attempt) {
        io::cancellation_source layoutChanged;
        pages = bukvitsa::Pages{};

        io::task paging = bukvitsa::paginate(layout, pages, layoutChanged.token());

        if (attempt < layoutChanges) {
            // Читатель потянул за угол окна: покрутили насос немного и сменили
            // кегль. Идущая пагинация после этого не нужна — она считала по
            // раскладке, которой больше нет.
            int slices = 0;
            loop.run_until([&] { return paging.done() || ++slices >= 25; });

            ++layout.fontSize;
            layoutChanged.cancel();
        }

        loop.run_until([&paging] { return paging.done(); });

        try {
            paging.result();
            paginated = true;
            std::println("");
            std::println("пагинация закончена: {} страниц, {} блоков", pages.counted,
                         pages.blocksDone);
        } catch (const wxl::async::operation_canceled_exception&) {
            std::println("");
            std::println("кегль стал {} — прервана на {} блоках, считаем заново", layout.fontSize,
                         pages.blocksDone);
        }
    }

    loop.run_until([&ui] { return ui.done(); });
    ui.result();
    std::println("");

    worker.stop_async().get();
    return 0;
}
