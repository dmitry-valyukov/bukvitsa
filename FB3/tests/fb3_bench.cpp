// Сколько стоит узнать о книге то, что нужно витрине.
//
// Вопрос, ради которого это написано: может ли скан каталога книг идти
// порциями прямо на потоке GUI, или он обязан уехать на пул. Ответ — в
// миллисекундах на книгу, и он разный для разных долей работы, поэтому
// замер разбит на четыре фазы:
//
//   пакет      — открыть OPC-контейнер (COM, чтение каталога zip);
//   описание   — найти часть с метаданными по типу связи и прочитать её байты;
//   книга      — Document целиком, то есть ещё и разбор тела: столько платит
//                витрина сегодня, потому что Library::add берёт готовый Book;
//   обложка    — достать байты картинки-обложки и записать их в кэш, как это
//                делает cacheCover.
//
// Числа — лучшее из нескольких прогонов, то есть по горячему кэшу файлов.
// Холодный первый скан добавит сверху чтение с диска.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

import wxl.core;

import bukvitsa.fb3;

using namespace bukvitsa::fb3;

namespace {

// Тот же тип связи, по которому Document находит точку входа: имена частей
// формат не фиксирует, угадывать их нельзя.
constexpr std::wstring_view kRelBook =
    L"http://www.fictionbook.org/FictionBook3/relationships/Book";

constexpr int kRepeats = 7;

/// Лучшее время из kRepeats прогонов, в миллисекундах.
template <class F>
double best(F&& body) {
    double result = 1e9;

    for (int i = 0; i < kRepeats; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        const auto finish = std::chrono::steady_clock::now();

        const double ms = std::chrono::duration<double, std::milli>(finish - start).count();
        result = std::min(result, ms);
    }

    return result;
}

void benchBook(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    std::printf("\n=== %s (%.1f МБ) ===\n", path.filename().string().c_str(),
                static_cast<double>(size) / (1024.0 * 1024.0));

    const double packageMs = best([&] { OpcPackage package(path); });

    // Пакет + метаданные: столько стоил бы вариант, который читает только
    // описание и не трогает тело.
    std::size_t descriptionBytes = 0;
    const double descriptionMs = best([&] {
        OpcPackage package(path);
        if (const auto part = package.partByPackageRelationship(kRelBook)) {
            descriptionBytes = package.readPart(*part).size();
        }
    });

    // Из чего складывается это время: поиск части по типу связи и чтение её
    // байтов — по отдельности, на уже открытом пакете.
    OpcPackage opened(path);
    const double lookupMs = best([&] { (void)opened.partByPackageRelationship(kRelBook); });

    const auto descriptionPart = opened.partByPackageRelationship(kRelBook);
    const double readPartMs =
        descriptionPart ? best([&] { (void)opened.readPart(*descriptionPart); }) : 0.0;

    // Ориентир снизу: во что обходится просто прочитать весь файл с диска
    // по горячему кэшу. Дешевле этого разбор стоить не может.
    const double rawReadMs = best([&] {
        std::FILE* file = nullptr;
        if (::_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
            std::vector<char> bytes(static_cast<std::size_t>(size));
            (void)std::fread(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
        }
    });

    const double documentMs = best([&] { Document book(path); });

    // Обложка — на уже открытой книге: её байты читаются лениво, при первом
    // обращении, поэтому в documentMs их нет.
    Document book(path);
    const auto coverIndex = book.description().coverImageIndex;

    double coverReadMs = 0;
    double coverWriteMs = 0;
    std::size_t coverBytes = 0;

    if (coverIndex) {
        // Первое обращение читает часть из пакета, дальше отдаётся готовое,
        // поэтому измеряется одна книга на прогон.
        coverReadMs = best([&] {
            Document fresh(path);
            if (const ImagePart* image = fresh.image(*coverIndex)) coverBytes = image->bytes.size();
        }) - documentMs;

        if (const ImagePart* image = book.image(*coverIndex)) {
            const std::filesystem::path out =
                std::filesystem::temp_directory_path() / L"bukvitsa_bench_cover.bin";

            coverWriteMs = best([&] {
                std::FILE* file = nullptr;
                if (::_wfopen_s(&file, out.c_str(), L"wb") == 0 && file) {
                    std::fwrite(image->bytes.data(), 1, image->bytes.size(), file);
                    std::fclose(file);
                }
            });

            std::error_code ignored;
            std::filesystem::remove(out, ignored);
        }
    }

    std::printf("  символов: %u, картинок: %zu, описание: %zu Б\n", book.characterCount(),
                book.images().size(), descriptionBytes);
    std::printf("  пакет:    %7.3f мс  (открыть контейнер OPC)\n", packageMs);
    std::printf("    связь:  %7.3f мс  (найти часть по типу связи)\n", lookupMs);
    std::printf("    байты:  %7.3f мс  (прочитать эту часть)\n", readPartMs);
    std::printf("  описание: %7.3f мс  (пакет + связь + байты)\n", descriptionMs);
    std::printf("  файл:     %7.3f мс  (fread целиком, ориентир снизу)\n", rawReadMs);
    std::printf("  книга:    %7.3f мс  (Document целиком, с телом)\n", documentMs);

    if (coverIndex) {
        std::printf("  обложка:  %7.3f мс чтение + %7.3f мс запись (%zu Б)\n",
                    coverReadMs > 0 ? coverReadMs : 0.0, coverWriteMs, coverBytes);
        std::printf("  ИТОГО на книгу при скане: %7.3f мс\n",
                    documentMs + (coverReadMs > 0 ? coverReadMs : 0.0) + coverWriteMs);
    } else {
        std::printf("  обложки нет\n");
        std::printf("  ИТОГО на книгу при скане: %7.3f мс\n", documentMs);
    }
}

}  // namespace

int main() {
    // Пул строится один раз на процесс и переживает всё, что из него берёт.
    wxl::core::sta_memory_pool pool;

    const std::filesystem::path testdata{BUKVITSA_TESTDATA_DIR};

    for (const char* name : {"anathomy_tutorial_example.fb3", "nightmare_example.fb3",
                             "hardcore_file_structure.fb3",
                             "Prokofev_R._Stellar9._Stellar_Prometeyi67901532.fb3"}) {
        const std::filesystem::path path = testdata / name;

        if (!std::filesystem::exists(path)) {
            std::printf("\n=== %s: нет файла, пропущен ===\n", name);
            continue;
        }

        try {
            benchBook(path);
        } catch (const std::exception& error) {
            std::printf("ОШИБКА %s: %s\n", name, error.what());
        }
    }

    return 0;
}
