// Проверка вёрстки на настоящих книгах. Как и у FB3, без фреймворка: ценность
// теста в том, что через него проходят реальные файлы.
//
// Пока проверяется первый слой — разворачивание дерева книги в блоки: порядок
// блоков, свёртка пробелов, прогоны стиля и таблица позиций.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>

#include <dwrite.h>
#include <wrl/client.h>

import wxl.core;

// Заголовки вёрстки после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/block.h"
#include "bukvitsa/typography/layout.h"
#include "bukvitsa/typography/page.h"

import bukvitsa.fb3;

using namespace bukvitsa;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    std::printf("%s %.*s\n", condition ? "  ok  " : "FAILED", static_cast<int>(what.size()),
                what.data());
    if (!condition) ++failures;
}

const char* nameOf(typography::BlockKind kind) {
    switch (kind) {
    case typography::BlockKind::Paragraph:    return "абзац";
    case typography::BlockKind::Title:        return "заголовок";
    case typography::BlockKind::Subtitle:     return "подзаголовок";
    case typography::BlockKind::Epigraph:     return "эпиграф";
    case typography::BlockKind::Annotation:   return "аннотация";
    case typography::BlockKind::Verse:        return "стих";
    case typography::BlockKind::Preformatted: return "преформат";
    case typography::BlockKind::Quote:        return "цитата";
    case typography::BlockKind::Subscription: return "подпись";
    case typography::BlockKind::ListItem:     return "пункт";
    case typography::BlockKind::Image:        return "картинка";
    case typography::BlockKind::Separator:    return "разделитель";
    }
    return "?";
}

/// UTF-16 обратно в UTF-8 — только чтобы напечатать в консоль.
std::string toUtf8(std::wstring_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t code = text[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size())
            code = 0x10000 + ((code - 0xD800) << 10) + (text[++i] - 0xDC00);

        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return out;
}

void testLayout(typography::Engine& engine, const std::vector<typography::Block>& blocks, float width);
void showFirstLines(typography::Engine& engine, const std::vector<typography::Block>& blocks, float width);
void testScaledShaping(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                       float width);
void testPagination(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                    std::uint32_t characterCount);void testChunkedPagination(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                           std::uint32_t characterCount);
void testDraftPagination(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                     std::uint32_t characterCount);
void testSeparatorAtPageBottom(typography::Engine& engine);

void testBook(typography::Engine& engine, const std::filesystem::path& path) {
    std::printf("\n=== %s ===\n", path.filename().string().c_str());

    fb3::Document book(path);
    const std::vector<typography::Block> blocks = typography::flatten(book.body());

    std::printf("  блоков: %zu\n", blocks.size());
    check(!blocks.empty(), "книга развёрнута в блоки");

    // Блоки идут в порядке чтения: позиция каждого следующего не меньше.
    bool ordered = true;
    for (std::size_t i = 1; i < blocks.size(); ++i)
        if (blocks[i].charOffset < blocks[i - 1].charOffset) ordered = false;
    check(ordered, "блоки в порядке чтения");

    // Прогоны стиля покрывают текст целиком и не пересекаются: на этом стоит
    // весь шейпинг, который дальше режет абзац по ним.
    bool spansCoverText = true;
    bool offsetsMatchText = true;
    for (const typography::Block& block : blocks) {
        if (block.paragraph.charOffsets.size() != block.paragraph.text.size()) offsetsMatchText = false;

        std::uint32_t at = 0;
        for (const typography::StyleSpan& span : block.paragraph.spans) {
            if (span.start != at) spansCoverText = false;
            at = span.start + span.length;
        }
        if (at != block.paragraph.text.size()) spansCoverText = false;
    }
    check(spansCoverText, "прогоны стиля покрывают текст без дыр и перекрытий");
    check(offsetsMatchText, "у каждого символа есть позиция в книге");

    // Свёртка пробелов: ни двух подряд, ни пробела по краям — кроме
    // преформата, где значимо всё.
    bool collapsed = true;
    for (const typography::Block& block : blocks) {
        if (block.kind == typography::BlockKind::Preformatted) continue;
        const std::wstring_view text = block.paragraph.text;
        if (!text.empty() && (text.front() == L' ' || text.back() == L' ')) collapsed = false;
        if (text.find(L"  ") != std::wstring::npos) collapsed = false;
    }
    check(collapsed, "пробелы свёрнуты");

    std::printf("  начало книги:\n");
    for (std::size_t i = 0; i < blocks.size() && i < 6; ++i) {
        const typography::Block& block = blocks[i];
        const std::string text = toUtf8(block.paragraph.text);

        std::string label = nameOf(block.kind);
        if (block.kind == typography::BlockKind::Title) label += std::to_string(block.level);

        std::printf("    [%s @%u] %.100s%s\n", label.c_str(), block.charOffset, text.c_str(),
                    text.size() > 100 ? "..." : "");
    }

    // Ширина полосы книжного разворота при кегле 20: около 60 знаков в строке,
    // как в бумажной книге. Вторая ширина — узкое окно, где абзац разбивается
    // куда чаще и вылезают все ошибки нарезки.
    testLayout(engine, blocks, 620.0f);
    testLayout(engine, blocks, 260.0f);
    showFirstLines(engine, blocks, 620.0f);
    testPagination(engine, blocks, book.characterCount());
    testChunkedPagination(engine, blocks, book.characterCount());
    testDraftPagination(engine, blocks, book.characterCount());
    testScaledShaping(engine, blocks, 620.0f);
}

/// Стиль блока — та же таблица, по которой потом верстает читалка. Здесь она
/// в тесте затем, чтобы проверять вёрстку на всех видах блоков сразу.
typography::ParagraphStyle styleFor(const typography::Block& block, float base) {
    typography::ParagraphStyle style;
    style.fontSize = base;
    style.lineHeight = 1.45f;

    switch (block.kind) {
    case typography::BlockKind::Title:
        style.fontSize = base * (block.level <= 1 ? 1.6f : 1.3f);
        style.alignment = typography::Alignment::Center;
        style.bold = true;
        break;
    case typography::BlockKind::Subtitle:
        style.alignment = typography::Alignment::Center;
        style.bold = true;
        break;
    case typography::BlockKind::Epigraph:
    case typography::BlockKind::Annotation:
        style.fontSize = base * 0.9f;
        style.italic = true;
        break;
    case typography::BlockKind::Verse:
        style.alignment = typography::Alignment::Left;
        break;
    case typography::BlockKind::Preformatted:
        style.alignment = typography::Alignment::Left;
        style.monospace = true;
        break;
    case typography::BlockKind::Subscription:
        style.alignment = typography::Alignment::Right;
        style.italic = true;
        break;
    default:
        style.firstLineIndent = base * 1.5f;
        break;
    }

    return style;
}

/// Вёрстка всей книги на заданной ширине полосы. Проверяются те свойства, без
/// которых страница не построится: строки не шире полосы, идут по порядку и
/// покрывают текст, у каждой есть высота.
void testLayout(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                float width) {
    const auto started = std::chrono::steady_clock::now();

    std::size_t lineCount = 0;
    std::size_t glyphCount = 0;
    bool inOrder = true;
    bool haveHeight = true;

    float worstOverflow = 0.0f;
    std::wstring worstLine;
    const char* worstKind = "";
    std::size_t worstRuns = 0, worstBlockLength = 0, worstSpans = 0;
    std::uint32_t worstStart = 0;
    std::wstring worstNeighbours;

    for (const typography::Block& block : blocks) {
        if (block.paragraph.text.empty())
            continue;

        const typography::ParagraphStyle style = styleFor(block, 20.0f);
        const std::vector<typography::Line> lines = engine.layout(block.paragraph, width, style);

        std::uint32_t reached = 0;
        for (const typography::Line& line : lines) {
            ++lineCount;
            for (const typography::GlyphRun& run : line.runs)
                glyphCount += run.glyphIndices.size();

            // Полторы десятых доли пикселя запаса: выключка распределяет
            // добавку в float и вправе промахнуться на последний разряд.
            if (line.width > width + 0.15f && line.width - width > worstOverflow) {
                worstOverflow = line.width - width;
                worstLine = block.paragraph.text.substr(line.textStart, line.textLength);
                worstStart = line.textStart;
                worstNeighbours = block.paragraph.text.substr(
                    line.textStart >= 6 ? line.textStart - 6 : 0,
                    line.textLength + 12);
                worstKind = nameOf(block.kind);
                worstRuns = line.runs.size();
                worstBlockLength = block.paragraph.text.size();
                worstSpans = block.paragraph.spans.size();
            }
            if (line.textStart < reached) inOrder = false;
            if (line.height <= 0.0f) haveHeight = false;

            reached = line.textStart + line.textLength;
        }

        if (reached > block.paragraph.text.size()) inOrder = false;
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started).count();

    std::printf("  вёрстка полосы %.0f: %zu строк, %zu глифов, %.0f мс\n", width, lineCount,
                glyphCount, elapsed);

    check(lineCount > 0, "книга разбита на строки");

    // Сноски. Знак сноски — это то, по чему читатель щёлкает, поэтому он
    // обязан существовать (в FB3 <note> сплошь и рядом пуст, и знак ставит
    // вёрстка), обязан попасть на строку и обязан вести к телу сноски.
    std::size_t anchors = 0, placed = 0, resolved = 0;
    std::wstring firstMarker;

    for (const typography::Block& block : blocks) {
        anchors += block.paragraph.notes.size();

        if (firstMarker.empty() && !block.paragraph.notes.empty()) {
            const typography::NoteAnchor& note = block.paragraph.notes.front();
            firstMarker = block.paragraph.text.substr(note.position, note.length);
        }

        const typography::ParagraphStyle style = styleFor(block, 20.0f);
        for (const typography::Line& line : engine.layout(block.paragraph, width, style)) {
            placed += line.notes.size();
            for (const typography::PlacedNote& note : line.notes)
                if (note.target) ++resolved;
        }
    }

    if (anchors > 0) {
        std::printf("  сносок: %zu, на строках: %zu, с телом: %zu, первый знак: «%.40s»\n", anchors,
                    placed, resolved, toUtf8(firstMarker).c_str());
        // Не «столько же»: знак, который автор написал сам, бывает длиной в
        // целый абзац и тогда достаётся каждой строке, через которую прошёл, —
        // щёлкнуть по нему можно в любой из них.
        check(placed >= anchors, "каждый знак сноски попал на строку");
        check(resolved == placed, "каждый знак сноски ведёт к телу");
    }

    // Ни одна строка не шире полосы — на этом стоит вся страница. Даже слово,
    // которое не разрывается нигде, вёрстка обязана разбить: строка, вылезшая
    // за поле, не только некрасива, но и не поместится в окно.
    if (worstOverflow > 0.0f)
        std::printf("    шире полосы на %.1f [%s, прогонов %zu, блок %zu символов, %zu стилей,\n"
                    "      строка с %u длиной %zu]:\n"
                    "      строка:  |%s|\n"
                    "      с краями: %s\n",
                    worstOverflow, worstKind, worstRuns, worstBlockLength, worstSpans, worstStart,
                    worstLine.size(), toUtf8(worstLine).c_str(), toUtf8(worstNeighbours).c_str());

    check(worstOverflow == 0.0f, "ни одна строка не шире полосы");
    check(inOrder, "строки идут по порядку и не выходят за абзац");
    check(haveHeight, "у каждой строки есть высота");
}

/// Первые строки книги как текст — самый прямой способ увидеть, что вёрстка
/// разбила абзац там, где надо.
void showFirstLines(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                    float width) {
    int shown = 0;

    for (const typography::Block& block : blocks) {
        if (block.paragraph.text.size() < 200 || shown > 0)
            continue;

        const typography::ParagraphStyle style = styleFor(block, 20.0f);
        const std::vector<typography::Line> lines = engine.layout(block.paragraph, width, style);

        std::printf("  абзац на полосе %.0f (отступ %.0f):\n", width, style.firstLineIndent);
        for (std::size_t i = 0; i < lines.size() && i < 8; ++i) {
            const typography::Line& line = lines[i];
            const std::wstring text = block.paragraph.text.substr(line.textStart, line.textLength);
            std::printf("    %6.1f |%s\n", line.width, toUtf8(text).c_str());
        }
        ++shown;
    }
}

/// Пагинация: раскладка строк по страницам и поиск страницы по позиции чтения.
///
/// Меряется отдельно от вёрстки не из любопытства: `setStyle` — это то, что
/// происходит на каждое нажатие Ctrl+«+», и вопрос «почему смена кегля
/// подтормаживает» решается тем, какая доля этого времени приходится на
/// шейпинг, а какая на саму раскладку.
void testPagination(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                    std::uint32_t characterCount) {
    typography::PageStyle style;
    style.width = 620.0f;
    style.height = 800.0f;
    style.fontSize = 20.0f;

    const auto started = std::chrono::steady_clock::now();
    typography::Paginator paginator(engine, blocks, characterCount);
    paginator.setStyle(style);
    const auto firstElapsed = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started).count();

    // Второй setStyle с другим кеглем — ровно то, что делает Ctrl+«+».
    style.fontSize = 21.0f;
    const auto restarted = std::chrono::steady_clock::now();
    paginator.setStyle(style);
    const auto againElapsed = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - restarted).count();

    std::printf("  пагинация: %zu страниц, %.0f мс; смена кегля: %.0f мс\n",
                paginator.pageCount(), firstElapsed, againElapsed);

    check(paginator.pageCount() > 0, "книга разложена по страницам");

    // Страницы идут по порядку и не теряют текст между собой: разрыв здесь
    // означал бы, что читатель, листая, пропустил кусок книги.
    bool ordered = true;
    bool nonEmpty = true;
    for (std::size_t i = 0; i < paginator.pageCount(); ++i) {
        const typography::Page& page = paginator.page(i);
        if (page.lines.empty() && page.images.empty()) nonEmpty = false;
        if (i > 0 && page.firstCharOffset < paginator.page(i - 1).firstCharOffset) ordered = false;
    }
    check(ordered, "страницы идут по порядку");
    check(nonEmpty, "пустых страниц нет");

    // Обратный переход: по позиции чтения находится страница, на которой эта
    // позиция и лежит. На этом стоит сохранение места при смене кегля.
    bool roundTrip = true;
    for (std::size_t i = 0; i < paginator.pageCount(); ++i) {
        const typography::Page& page = paginator.page(i);
        if (paginator.pageForCharOffset(page.firstCharOffset) != i) roundTrip = false;
    }
    check(roundTrip, "по позиции чтения находится своя страница");

    // Блоки, отданные наружу: на них стоят оглавление, поиск и переход по
    // закладке. Проверяется, что это те же блоки, что верстались, и что
    // позиции в них не убывают -- то, на что опирается поиск места по книге.
    const std::span<const typography::Block> exposed = paginator.blocks();
    bool offsetsRise = true;
    for (std::size_t i = 1; i < exposed.size(); ++i) {
        if (exposed[i].charOffset < exposed[i - 1].charOffset) offsetsRise = false;
    }
    check(exposed.size() == blocks.size(), "пагинатор отдаёт те же блоки, из которых верстал");
    check(offsetsRise, "позиции блоков идут по возрастанию");
}

/// Порционная вёрстка: та же книга, разложенная не разом, а порциями.
///
/// Проверяется главное обещание порций: страницы выходят те же самые, а уже
/// набранные не переписываются задним числом. На втором стоит то, ради чего
/// порции и заведены, — читалка показывает страницу, не дожидаясь конца
/// счёта, и показанное потом не должно оказаться неправдой.
void testChunkedPagination(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                           std::uint32_t characterCount) {
    typography::PageStyle style;
    style.width = 620.0f;
    style.height = 800.0f;
    style.fontSize = 20.0f;

    typography::Paginator paginator(engine, blocks, characterCount);
    paginator.setStyle(style);

    // Снимок целой вёрстки. Именно снимок, а не ссылки: следующая вёрстка
    // перевернёт под ними страницы.
    struct Snapshot {
        std::uint32_t first;
        std::uint32_t last;
        std::size_t lines;
        std::size_t images;
    };
    std::vector<Snapshot> whole;
    for (std::size_t i = 0; i < paginator.pageCount(); ++i) {
        const typography::Page& page = paginator.page(i);
        whole.push_back({page.firstCharOffset, page.lastCharOffset, page.lines.size(),
                         page.images.size()});
    }

    // Совпала ли набранная порциями книга с той, что вышла разом.
    const auto matches = [&] {
        if (paginator.pageCount() != whole.size()) return false;
        for (std::size_t i = 0; i < whole.size(); ++i) {
            const typography::Page& page = paginator.page(i);
            if (page.firstCharOffset != whole[i].first || page.lastCharOffset != whole[i].last ||
                page.lines.size() != whole[i].lines || page.images.size() != whole[i].images)
                return false;
        }
        return true;
    };

    // Всё, что набрано к этой минуте, обязано совпадать с целой вёрсткой:
    // набранная страница окончательна и задним числом не переписывается.
    bool settled = true;
    bool grows = true;
    std::size_t seen = 0;
    const auto watch = [&] {
        if (paginator.pageCount() < seen) grows = false;
        for (std::size_t i = 0; i < seen && i < whole.size(); ++i) {
            const typography::Page& page = paginator.page(i);
            if (page.firstCharOffset != whole[i].first || page.lines.size() != whole[i].lines)
                settled = false;
        }
        seen = paginator.pageCount();
    };

    // Порция в ноль: срок вышел ещё до начала, значит каждая считает ровно
    // один блок. Так меряется то, чем порция может превысить свой срок, —
    // цена самого дорогого блока книги; разорвать его нечем.
    double worstBlock = 0.0;
    paginator.beginLayout(style);
    check(!paginator.isComplete() || blocks.empty(), "начатая вёрстка ещё не досчитана");

    while (true) {
        const auto started = std::chrono::steady_clock::now();
        const bool more = paginator.advance(std::chrono::milliseconds(0));
        worstBlock = std::max(worstBlock, std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - started).count());
        watch();
        if (!more) break;
    }
    check(matches(), "книга, свёрстанная по блоку за раз, — та же самая");

    // А теперь порция та, что берёт читалка.
    const auto slice = std::chrono::milliseconds(50);
    double worstChunk = 0.0;
    double wholeElapsed = 0.0;
    int chunks = 0;
    seen = 0;
    paginator.beginLayout(style);

    while (true) {
        const auto started = std::chrono::steady_clock::now();
        const bool more = paginator.advance(slice);
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started).count();
        worstChunk = std::max(worstChunk, elapsed);
        wholeElapsed += elapsed;
        ++chunks;
        watch();
        if (!more) break;
    }

    std::printf("  порциями: %d порций по 50 мс, худшая %.0f мс из %.0f, дороже всех блок %.0f\n",
                chunks, worstChunk, wholeElapsed, worstBlock);

    check(paginator.isComplete(), "книга досчитана");
    check(grows, "число страниц только растёт");
    check(settled, "набранная страница больше не меняется");
    check(matches(), "порциями выходит ровно та же книга, что и разом");

    // Обещание порции: она кончается на первом блоке после срока. Проверяется
    // это числом порций — без срока порция была бы одна на всю книгу, — а не
    // самими миллисекундами: они зависят от сборки и от того, чем ещё занята
    // машина, и сравнивать два замера между собой значило бы ловить чужую
    // загрузку. Сколько порция стоит на самом деле, записано в
    // docs/decisions.md.
    check(chunks > 1 || wholeElapsed < 50.0, "книга, не влезшая в срок, разбита на порции");

    // И грубый предел сверху — на случай, если срок перестанут проверять
    // вовсе: тогда порция стала бы всей книгой.
    check(worstChunk <= 50.0 + worstBlock * 3.0 + 25.0, "порция не растягивается на всю книгу");
}

/// Грязная вёрстка: то, что читатель видит в тот же кадр, в котором сменил
/// кегль или размер окна, и то, чем он листает вперёд, пока книга набирается
/// начисто.
void testDraftPagination(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                         std::uint32_t characterCount) {
    typography::PageStyle style;
    style.width = 620.0f;
    style.height = 800.0f;
    style.fontSize = 20.0f;

    typography::Paginator paginator(engine, blocks, characterCount);
    paginator.setStyle(style);
    if (paginator.pageCount() < 8) {
        check(true, "книга слишком коротка для грязной вёрстки — пропущено");
        return;
    }

    // Место чтения берётся из настоящей вёрстки: так оно и приходит из
    // читалки — первым символом страницы, на которой читатель стоял.
    const std::uint32_t at = paginator.page(paginator.pageCount() / 2).firstCharOffset;

    // И кегль другой: грязная вёрстка затем и нужна, что полоса сменилась.
    style.fontSize = 23.0f;

    const auto started = std::chrono::steady_clock::now();
    paginator.draftAt(style, at, 2);
    const double elapsed = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - started).count();

    std::printf("  грязная вёрстка: %zu страниц с символа %u, %.2f мс\n", paginator.draftCount(),
                at, elapsed);

    check(paginator.draftCount() >= 2, "грязная вёрстка даёт столько страниц, сколько просили");
    check(paginator.draftPage(0).firstCharOffset == at,
          "грязная страница начинается ровно с места чтения");
    check(paginator.draftPage(1).firstCharOffset > at, "вторая страница идёт за первой");

    // Полоса та же, что и у книжной вёрстки: грязная страница не имеет права
    // быть длиннее той, которую читатель увидит потом.
    bool inside = true;
    for (std::size_t i = 0; i < paginator.draftCount(); ++i) {
        for (const typography::PlacedLine& placed : paginator.draftPage(i).lines) {
            if (placed.baseline > style.height || placed.line == nullptr) inside = false;
        }
    }
    check(inside, "строки грязной страницы не вылезают за полосу");

    // Листание вперёд. Страницу за показанными грязная вёрстка досчитывает
    // тогда, когда её спросили, а не заранее: перевёрстка случается на каждое
    // движение мыши, листание — куда реже. Главное здесь то, что текст не
    // теряется и не повторяется: новый разворот начинается ровно там, где
    // кончился прежний.
    bool walks = true;
    int steps = 0;

    for (; steps < 24; ++steps) {
        if (!paginator.draftUpTo(3)) break;   // книга кончилась

        const std::uint32_t next = paginator.draftPage(2).firstCharOffset;
        if (next <= paginator.draftPage(1).lastCharOffset) walks = false;

        paginator.draftAt(style, next, 2);
        if (paginator.draftCount() == 0 || paginator.draftPage(0).firstCharOffset != next)
            walks = false;
    }

    check(steps > 0, "по книге можно листать грязной вёрсткой");
    check(walks, "следующий разворот начинается там, где кончился прежний");

    // Досчёт до конца книги: грязная вёрстка честно говорит, что дальше
    // страниц нет, а не отдаёт пустую.
    paginator.draftAt(style, paginator.page(paginator.pageCount() - 1).firstCharOffset, 2);
    const bool beyond = paginator.draftUpTo(64);
    check(!beyond, "за концом книги грязных страниц не выдумывается");

    // Энергичный досчёт: чистовой набор доводится ровно до нужного места и не
    // дальше. На этом стоит листание назад и прыжок по закладке — читателю
    // там ждать порций нечего, но и считать книгу целиком незачем.
    typography::Paginator second(engine, blocks, characterCount);
    second.beginLayout(style);
    const bool more = second.advanceTo(at);
    const std::size_t reached = second.pageForCharOffset(at);

    check(second.page(reached).firstCharOffset <= at, "досчитано до места чтения");
    check(second.pageCount() > reached, "страница с этим символом окончательна");

    second.advanceToPage(reached + 2);
    check(second.pageCount() > reached + 2, "досчитано и до всего разворота");

    std::printf("  энергичный досчёт: %zu страниц из %zu, книга %s\n", second.pageCount(),
                paginator.pageCount(), more ? "ещё не досчитана" : "досчитана целиком");
}
/// Разделитель, пришедшийся на низ полосы, не должен зацикливать набор.
///
/// Отбивка разделителя занимает место, но страницы не начинает: страница
/// остаётся пустой, а места на ней уже нет. Строка, которая в остаток не
/// влезла, требует новой страницы — а закрывать нечего, и набор до
/// исправления ходил по кругу вечно.
void testSeparatorAtPageBottom(typography::Engine& engine) {
    std::vector<typography::Block> blocks;

    typography::Block separator;
    separator.kind = typography::BlockKind::Separator;
    blocks.push_back(separator);

    typography::Block paragraph;
    paragraph.kind = typography::BlockKind::Paragraph;
    paragraph.paragraph.text = L"Строка, которой уже не хватает места на полосе.";
    paragraph.paragraph.charOffsets.resize(paragraph.paragraph.text.size());
    for (std::uint32_t i = 0; i < paragraph.paragraph.charOffsets.size(); ++i)
        paragraph.paragraph.charOffsets[i] = i;
    blocks.push_back(paragraph);

    typography::PageStyle style;
    style.width = 400.0f;
    style.fontSize = 20.0f;
    // Полоса ровно в одну строку: отбивка разделителя — полтора кегля — и
    // строка на неё вместе уже не помещаются.
    style.height = 30.0f;

    typography::Paginator paginator(
        engine, blocks, static_cast<std::uint32_t>(paragraph.paragraph.text.size()));
    paginator.setStyle(style);

    std::printf("\n=== узкая полоса ===\n");
    check(paginator.pageCount() >= 1, "разделитель у низа полосы не зацикливает набор");
}
/// Смена кегля через сохранённый шейпинг должна давать ровно то же, что
/// шейпинг заново на новом кегле.
///
/// Это проверка допущения, на котором стоит весь кэш: метрики DirectWrite в
/// естественном режиме измерения линейны по кеглю, поэтому строку при кегле 26
/// можно получить из шейпинга при кегле 20 умножением. Если допущение неверно,
/// разойдутся переломы — и разойдутся заметно, а не в последнем разряде.
void testScaledShaping(typography::Engine& engine, const std::vector<typography::Block>& blocks,
                       float width) {
    std::size_t compared = 0;
    std::size_t sameBreaks = 0;
    float worstWidth = 0.0f;

    for (const typography::Block& block : blocks) {
        if (block.paragraph.text.empty())
            continue;

        const typography::ParagraphStyle atTwenty = styleFor(block, 20.0f);
        const typography::ParagraphStyle atTwentySix = styleFor(block, 26.0f);

        const typography::ShapedParagraphPtr shaped = engine.shape(block.paragraph, atTwenty);
        const std::vector<typography::Line> scaled = engine.layout(*shaped, width, atTwentySix);
        const std::vector<typography::Line> fresh = engine.layout(block.paragraph, width, atTwentySix);

        ++compared;
        if (scaled.size() != fresh.size())
            continue;

        bool same = true;
        for (std::size_t i = 0; i < scaled.size(); ++i) {
            if (scaled[i].textStart != fresh[i].textStart ||
                scaled[i].textLength != fresh[i].textLength)
                same = false;
            worstWidth = std::max(worstWidth, std::abs(scaled[i].width - fresh[i].width));
        }
        if (same)
            ++sameBreaks;
    }

    std::printf("  кегль из кэша: %zu абзацев, совпало %zu, худший разброс ширины %.4f\n",
                compared, sameBreaks, worstWidth);

    check(compared == 0 || sameBreaks == compared,
          "вёрстка из сохранённого шейпинга совпадает с вёрсткой заново");
    check(worstWidth < 0.05f, "ширины строк совпадают");
}

}  // namespace

int main() {
    wxl::core::sta_memory_pool pool;

    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
        std::printf("FAILED не удалось создать фабрику DirectWrite\n");
        return 1;
    }

    typography::Engine engine(dwrite.Get());

    testSeparatorAtPageBottom(engine);

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
            testBook(engine, path);
        } catch (const std::exception& error) {
            std::printf("FAILED %s: %s\n", name, error.what());
            ++failures;
        }
    }

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
