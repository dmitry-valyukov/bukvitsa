// Проверка загрузки книги на официальных примерах из репозитория
// спецификации. Без фреймворка: тест ценен тем, что читает настоящие
// файлы, а не тем, как он об этом сообщает.
//
// wxl.xml выделяет память из STA-пула wxl.core — он должен существовать
// до первого разбора и пережить последнюю книгу.

#include <cstdio>
#include <filesystem>
#include <string>

import wxl.core;

import bukvitsa.fb3;

using namespace bukvitsa::fb3;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    std::printf("%s %.*s\n", condition ? "  ok  " : "FAILED", static_cast<int>(what.size()),
                what.data());
    if (!condition) ++failures;
}

/// Сколько узлов каждого рода в книге — самый быстрый способ увидеть,
/// что разбор не потерял и не выдумал структуру.
void countNodes(const Node& node, int counts[64]) {
    counts[static_cast<int>(node.kind())]++;
    for (const Node& child : node.children())
        countNodes(child, counts);
}

/// Первые символы книги как их увидит вёрстка: текст в порядке документа.
std::string firstText(const Node& node, std::size_t limit) {
    std::string text;

    auto walk = [&](const Node& n, auto& self) -> void {
        if (text.size() >= limit) return;
        if (n.kind() == NodeKind::Text) text += n.text().chars();
        for (const Node& child : n.children()) self(child, self);
    };

    walk(node, walk);
    return text.substr(0, limit);
}

void testBook(const std::filesystem::path& path) {
    std::printf("\n=== %s ===\n", path.filename().string().c_str());

    Document book(path);
    const Description& d = book.description();

    std::printf("  название: %s\n", d.title.c_str());
    std::printf("  авторы:   %s\n", d.authorsLine().c_str());
    std::printf("  язык:     %s, символов: %u, картинок: %zu\n",
                d.language.c_str(), book.characterCount(), book.images().size());

    check(!d.title.empty(), "название прочитано");
    check(book.characterCount() > 0, "в книге есть текст");
    check(book.body().hasChildren(), "у тела есть дети");

    int counts[64] = {};
    countNodes(book.body(), counts);
    std::printf("  секций: %d, абзацев: %d, текстовых узлов: %d, картинок в тексте: %d\n",
                counts[static_cast<int>(NodeKind::Section)], counts[static_cast<int>(NodeKind::Paragraph)],
                counts[static_cast<int>(NodeKind::Text)], counts[static_cast<int>(NodeKind::Image)]);

    check(counts[static_cast<int>(NodeKind::Section)] > 0, "секции разобраны");
    check(counts[static_cast<int>(NodeKind::Paragraph)] > 0, "абзацы разобраны");

    std::printf("  начало:   %s...\n", firstText(book.body(), 120).c_str());

    // Позиция чтения: по смещению символа находится узел, и он не позже него.
    if (const Node* at = book.nodeAtCharOffset(book.characterCount() / 2))
        check(at->charOffset() <= book.characterCount() / 2, "позиция чтения находит узел");

    // Сноски: каждая ссылка нашла своё тело — иначе при вёрстке некуда
    // будет отправить читателя.
    int refs = 0, resolved = 0;
    auto walkNotes = [&](const Node& n, auto& self) -> void {
        if (const NoteRefData* ref = n.noteRef()) {
            ++refs;
            if (ref->target) ++resolved;
        }
        for (const Node& child : n.children()) self(child, self);
    };
    walkNotes(book.body(), walkNotes);

    if (refs > 0) {
        std::printf("  сносок: %d, разрешено: %d\n", refs, resolved);
        check(refs == resolved, "все ссылки на сноски разрешены");
    }
}

}  // namespace

int main() {
    // Пул строится один раз на процесс и переживает всё, что из него берёт.
    wxl::core::sta_memory_pool pool;

    const std::filesystem::path testdata{BUKVITSA_TESTDATA_DIR};

    // Три официальных примера из репозитория спецификации плюс, если он лежит
    // рядом, настоящий файл ЛитРес: официальные примеры проверяют формат,
    // а книга из магазина — то, что издатель на самом деле пишет.
    for (const char* name : {"anathomy_tutorial_example.fb3", "nightmare_example.fb3",
                             "hardcore_file_structure.fb3",
                             "Prokofev_R._Stellar9._Stellar_Prometeyi67901532.fb3"}) {
        const std::filesystem::path path = testdata / name;

        if (!std::filesystem::exists(path)) {
            std::printf("\n=== %s: нет файла, пропущен ===\n", name);
            continue;
        }

        try {
            testBook(path);
        } catch (const std::exception& error) {
            std::printf("FAILED %s: %s\n", name, error.what());
            ++failures;
        }
    }

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
