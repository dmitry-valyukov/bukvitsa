// Пагинация: блоки -> строки -> страницы.
//
// Два прохода. Первый верстает каждый блок в строки и запоминает, сколько
// места он просит сверху и снизу. Второй набирает из этих строк страницы,
// следя за тем, чтобы заголовок не оставался один внизу полосы, а абзац не
// отдавал следующей странице одну свою строку.
//
// Оба прохода умеют останавливаться и продолжать. Книга верстается порциями,
// потому что верстать её целиком — это доли секунды, а доли секунды на потоке
// окна читатель видит как заедание: он тянет край окна или крутит колесо с
// Ctrl, и каждое движение стоило бы ему целой книги. Поэтому состояние набора
// вынесено из функции в `PageBuilder`: между порциями ему надо где-то лежать.
//
// Отдельно от порций стоит грязная вёрстка (`draftAt`) — те несколько страниц,
// которые читатель видит прямо сейчас, свёрстанные ровно с его места и ни от
// чего больше не зависящие. Она считается за единицы миллисекунд и потому
// показывается сразу, а книга набирается следом, порциями, в свободное время
// потока — и подменять собой показанное не спешит.
//
// Грязная вёрстка продолжаема: `draftUpTo` досчитывает её до нужного числа
// страниц, начиная с того блока, на котором она остановилась. Так листание
// вперёд получает следующую страницу тогда, когда она понадобилась, а не
// заранее — считать её на каждое движение мыши значило бы считать зря.
//
// Стиль блока — таблица здесь, а не в приложении: как выглядит эпиграф, знает
// вёрстка, а не окно. Читалка задаёт кегль и полосу, всё остальное отсюда.

#include <algorithm>
#include <optional>

// Свой заголовок после всех стандартных: он ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/page.h"

namespace bukvitsa::typography {
namespace {

/// Блок, разложенный на строки, вместе с отбивками.
struct LaidOutBlock {
    const Block* source = nullptr;
    std::vector<Line> lines;
    float indent = 0.0f;        ///< втяжка блока слева
    float spaceBefore = 0.0f;
    float spaceAfter = 0.0f;

    ImageSize image;            ///< для BlockKind::Image
    bool isImage = false;

    /// Насколько нежелательно оторвать этот блок от следующего. Заголовок
    /// внизу полосы — типографский брак: читатель перевернёт страницу и не
    /// поймёт, к чему заголовок относился.
    bool keepWithNext = false;

    /// Есть ли этому блоку что поставить на полосу. Пустой блок — это
    /// разделитель или ещё не свёрстанный: он занимает только отбивку.
    bool occupies() const { return isImage || !lines.empty(); }
};

ParagraphStyle styleFor(const Block& block, const PageStyle& page) {
    ParagraphStyle style;
    style.fontSize = page.fontSize;
    style.lineHeight = page.lineHeight;

    switch (block.kind) {
    case BlockKind::Title:
        // Заголовок первого уровня — имя книги или части, он крупнее прочих.
        style.fontSize = page.fontSize * (block.level <= 1 ? 1.55f : 1.25f);
        style.alignment = Alignment::Center;
        style.bold = true;
        break;

    case BlockKind::Subtitle:
        style.fontSize = page.fontSize * 1.1f;
        style.alignment = Alignment::Center;
        style.bold = true;
        break;

    case BlockKind::Epigraph:
        style.fontSize = page.fontSize * 0.92f;
        style.italic = true;
        style.alignment = Alignment::Justify;
        break;

    case BlockKind::Annotation:
        style.fontSize = page.fontSize * 0.92f;
        style.italic = true;
        break;

    case BlockKind::Subscription:
        style.fontSize = page.fontSize * 0.92f;
        style.italic = true;
        style.alignment = Alignment::Right;
        break;

    case BlockKind::Verse:
        // Стих не выключают и не переносят: строка кончается там, где её
        // кончил поэт, а не там, где кончилась полоса.
        style.alignment = Alignment::Left;
        break;

    case BlockKind::Preformatted:
        style.alignment = Alignment::Left;
        style.monospace = true;
        style.fontSize = page.fontSize * 0.9f;
        break;

    case BlockKind::Quote:
        style.alignment = Alignment::Justify;
        style.firstLineIndent = page.fontSize * page.paragraphIndent;
        break;

    case BlockKind::ListItem:
        style.alignment = Alignment::Justify;
        break;

    default:
        style.alignment = Alignment::Justify;
        style.firstLineIndent = page.fontSize * page.paragraphIndent;
        break;
    }

    return style;
}

/// Втяжка блока слева — на сколько его полоса уже книжной.
float indentFor(const Block& block, const PageStyle& page) {
    switch (block.kind) {
    case BlockKind::Quote:
        return page.fontSize * 2.0f;
    case BlockKind::Epigraph:
        return page.fontSize * 4.0f;
    case BlockKind::Verse:
        return page.fontSize * 2.0f;
    case BlockKind::ListItem:
        return page.fontSize * (1.5f * (block.listDepth == 0 ? 1 : block.listDepth));
    default:
        return 0.0f;
    }
}

/// Отбивка сверху и снизу, в кеглях. Заголовку нужен воздух, абзацу — нет:
/// в книге абзацы отделяет отступ первой строки, а не пустое место.
struct Spacing {
    float before;
    float after;
};

Spacing spacingFor(const Block& block) {
    switch (block.kind) {
    case BlockKind::Title:
        return {1.6f, 1.0f};
    case BlockKind::Subtitle:
        return {1.2f, 0.6f};
    case BlockKind::Epigraph:
    case BlockKind::Annotation:
        return {0.8f, 0.8f};
    case BlockKind::Subscription:
        return {0.4f, 0.8f};
    case BlockKind::Quote:
    case BlockKind::Preformatted:
        return {0.6f, 0.6f};
    case BlockKind::Verse:
        return {0.0f, 0.0f};
    case BlockKind::Image:
        return {0.8f, 0.8f};
    case BlockKind::Separator:
        return {0.8f, 0.8f};
    default:
        return {0.0f, 0.0f};
    }
}

/* ================================================================== */

/// Набор страниц из свёрстанных блоков — второй проход, оформленный так,
/// чтобы его можно было остановить на любом блоке и продолжить с него же.
///
/// Владение: набор смотрит в чужой список блоков и складывает в свои страницы
/// указатели на его строки. Значит, список обязан пережить набор и не менять
/// адресов уже поставленных строк; оба места, откуда набор зовут, это
/// обеспечивают.
struct PageBuilder {
    const std::vector<LaidOutBlock>* source = nullptr;
    PageStyle style;

    std::vector<Page> pages;
    Page current;
    float used = 0.0f;              ///< сколько полосы занято сверху
    std::uint32_t lastOffset = 0;   ///< позиция последнего поставленного
    std::size_t block = 0;          ///< блок, который ставится следующим
    std::size_t line = 0;           ///< строка внутри него

    void reset(const std::vector<LaidOutBlock>& blocks, const PageStyle& pageStyle) {
        source = &blocks;
        style = pageStyle;
        pages.clear();
        current = Page{};
        used = 0.0f;
        lastOffset = 0;
        block = 0;
        line = 0;
    }

    /// Сколько места на полосе занимает строка с номером `index`, считая
    /// отбивку перед блоком, если строка первая.
    float heightOf(const LaidOutBlock& item, std::size_t index) const {
        float height = item.lines[index].height;
        if (index == 0)
            height += item.spaceBefore;
        return height;
    }

    /// Помещаются ли на оставшейся высоте хотя бы `count` строк блока подряд,
    /// начиная с `from`.
    bool fitsFromHere(const LaidOutBlock& item, std::size_t from, float available,
                      std::size_t count) const {
        float needed = 0.0f;
        for (std::size_t i = from; i < item.lines.size() && i < from + count; ++i)
            needed += heightOf(item, i);
        return needed <= available;
    }

    /// Влезет ли за заголовком хотя бы одна строка следующего блока.
    bool nextBlockHasRoom(std::size_t index, float taken) const {
        for (std::size_t next = index + 1; next < source->size(); ++next) {
            const LaidOutBlock& item = (*source)[next];
            if (item.isImage)
                return taken + item.spaceBefore + item.image.height <= style.height;
            if (item.lines.empty())
                continue;
            return taken + heightOf(item, 0) <= style.height;
        }
        return true;   // за заголовком ничего нет — держать не за что
    }

    void flushPage() {
        if (!current.lines.empty() || !current.images.empty()) {
            current.lastCharOffset = lastOffset;
            pages.push_back(std::move(current));
            current = Page{};
        }

        // Отбивка, накопленная на пустой полосе, на новую страницу не
        // переносится — и не должна: разделитель, пришедшийся на низ полосы,
        // ничего не отбивает. Обнуляется безусловно ещё и потому, что иначе
        // строка, не влезающая в остаток, гоняла бы набор по кругу: полоса
        // пуста, сбрасывать нечего, а место всё занято.
        used = 0.0f;
    }

    void startOfPage(std::uint32_t offset) {
        if (current.lines.empty() && current.images.empty())
            current.firstCharOffset = offset;
    }

    /// Ставит на полосу блоки от того, на котором остановились, до `limit`.
    ///
    /// `limit` не должен заходить дальше последнего блока, за которым есть
    /// ещё один непустой: набор заглядывает вперёд — заголовок держится за
    /// то, что за ним, — и заглянуть в несвёрстанное значило бы принять
    /// решение по пустому месту.
    void place(std::size_t limit) {
        const std::vector<LaidOutBlock>& blocks = *source;

        while (block < limit) {
            const LaidOutBlock& item = blocks[block];

            // Глава начинается с новой страницы. Перед первым блоком главы
            // верхнего уровня закрываем полосу, если на ней уже что-то есть;
            // пустую не трогаем — иначе первая глава книги гнала бы за собой
            // пустую страницу. Подсекции (startsSection > 1) не разрывают.
            if (item.source && item.source->startsSection == 1 && used > 0.0f)
                flushPage();

            if (item.isImage) {
                if (item.image.height > 0.0f) {
                    const float needed = item.spaceBefore + item.image.height + item.spaceAfter;
                    if (used > 0.0f && used + needed > style.height)
                        flushPage();

                    startOfPage(item.source->charOffset);
                    current.images.push_back(PlacedImage{item.source->imageIndex,
                                                         (style.width - item.image.width) * 0.5f,
                                                         used + item.spaceBefore, item.image.width,
                                                         item.image.height});
                    used += needed;
                    lastOffset = item.source->charOffset;
                }
                ++block;
                continue;
            }

            if (item.lines.empty()) {
                // Разделитель: пустое место, но не начало новой страницы.
                used += item.spaceBefore + item.spaceAfter;
                ++block;
                continue;
            }

            while (line < item.lines.size()) {
                const float height = heightOf(item, line);

                if (used > 0.0f && used + height > style.height) {
                    flushPage();
                    continue;   // ту же строку — уже на новую страницу
                }

                // Висячая строка: одна строка абзаца внизу полосы, а остальные
                // на следующей. Отправляем весь абзац дальше — но только если
                // он не занимает полосу целиком, иначе будем гонять его вечно.
                if (line == 0 && used > 0.0f && item.lines.size() > 1 &&
                    !fitsFromHere(item, 0, style.height - used, 2) &&
                    fitsFromHere(item, 0, style.height, 2)) {
                    flushPage();
                    continue;
                }

                // Заголовок, оставшийся один внизу полосы.
                if (item.keepWithNext && used > 0.0f && line + 1 == item.lines.size() &&
                    !nextBlockHasRoom(block, used + height)) {
                    flushPage();
                    continue;
                }

                startOfPage(item.lines[line].charOffset);

                current.lines.push_back(PlacedLine{&item.lines[line], item.indent,
                                                   used + (line == 0 ? item.spaceBefore : 0.0f) +
                                                       item.lines[line].ascent});
                used += height;
                lastOffset = item.lines[line].charOffset;
                ++line;
            }

            used += item.spaceAfter;
            ++block;
            line = 0;
        }
    }

    /// Закрывает последнюю страницу. Зовётся, когда ставить больше нечего.
    void finish() { flushPage(); }
};

/// За какой блок набору заходить нельзя: индекс последнего непустого в списке.
/// Ноль означает «пока некуда» — и это верно и для пустого списка, и для
/// списка из одних разделителей.
std::size_t nonEmptyLimit(const std::vector<LaidOutBlock>& blocks) {
    for (std::size_t i = blocks.size(); i > 0; --i) {
        if (blocks[i - 1].occupies())
            return i - 1;
    }
    return 0;
}

}  // namespace

/* ================================================================== */

struct Paginator::Impl {
    Engine& engine;
    std::span<const Block> blocks;   ///< книги: вид в мастер-список Book, не копия
    std::function<ImageSize(std::uint32_t)> imageSize;

    PageStyle style;
    std::uint32_t characterCount = 0;

    /// Шейпинг каждого блока, по индексу в blocks. Делается один раз и живёт
    /// столько же, сколько книга: от кегля и полосы он не зависит, а стоит
    /// почти всей вёрстки — 253 мс из 267 на романе в 650 тысяч знаков.
    /// Ради этого и разделены shape и layout.
    std::vector<ShapedParagraphPtr> shaped;

    /* ---------------- порционная вёрстка книги ---------------- */

    /// Свёрстанные блоки книги, по индексу в blocks. Заводится сразу на всю
    /// книгу и не растёт: страницы держат указатели на строки, и всякое
    /// перевыделение сделало бы их недействительными.
    std::vector<LaidOutBlock> laidOut;
    PageBuilder book;
    std::size_t layoutCursor = 0;    ///< блок, который верстается следующим
    std::size_t lastNonEmpty = 0;    ///< за него набору заходить нельзя
    bool complete = true;

    /* ---------------- грязная вёрстка ---------------- */

    /// Своё хранилище, а не общее с книгой: грязная страница живёт ровно
    /// тогда, когда книга ещё считается, — то есть когда `laidOut` под ней
    /// перевёрстывается. Общее хранилище означало бы, что читатель смотрит на
    /// строки, которые в этот момент переписывают.
    std::vector<LaidOutBlock> draftBlocks;
    PageBuilder draft;
    std::size_t draftCursor = 0;        ///< следующий блок книги для грязной вёрстки
    std::uint32_t draftFirstChar = 0;   ///< с какого символа начинать этот блок
    bool draftDone = true;              ///< книга кончилась, страниц больше не будет

    Impl(Engine& engine_, std::span<const Block> blocks_, std::uint32_t characterCount_,
         std::function<ImageSize(std::uint32_t)> imageSize_)
        : engine(engine_), blocks(blocks_), imageSize(std::move(imageSize_)),
          characterCount(characterCount_) {}

    /* ---------------- вёрстка блока ---------------- */

    /// Картинка вписывается в полосу с сохранением пропорций и не занимает
    /// больше двух третей высоты: страница, целиком отданная под иллюстрацию,
    /// рвёт чтение.
    ImageSize fitImage(std::uint32_t index) const {
        ImageSize size = imageSize ? imageSize(index) : ImageSize{};
        if (size.width <= 0.0f || size.height <= 0.0f)
            return ImageSize{0.0f, 0.0f};

        const float maxWidth = style.width;
        const float maxHeight = style.height * 0.66f;
        const float scale = std::min({1.0f, maxWidth / size.width, maxHeight / size.height});

        return ImageSize{size.width * scale, size.height * scale};
    }

    /// Верстает один блок в строки.
    ///
    /// @param firstChar символ внутри абзаца, с которого начинать. Не ноль
    ///        только у первого блока мгновенной страницы: она начинается с
    ///        места чтения, а не с красной строки.
    LaidOutBlock layOut(std::size_t index, std::uint32_t firstChar) {
        const Block& block = blocks[index];

        LaidOutBlock item;
        item.source = &block;
        item.indent = indentFor(block, style);

        const Spacing spacing = spacingFor(block);
        // Абзац, начатый с середины, ничего сверху не отбивает: отбивка
        // принадлежит началу блока, а оно осталось на прошлой странице.
        item.spaceBefore = firstChar == 0 ? spacing.before * style.fontSize : 0.0f;
        item.spaceAfter = spacing.after * style.fontSize;

        if (block.kind == BlockKind::Image) {
            item.isImage = true;
            item.image = fitImage(block.imageIndex);
            return item;
        }

        if (block.kind == BlockKind::Separator)
            return item;

        const float measure = std::max(style.width - item.indent, style.fontSize * 4.0f);
        const ParagraphStyle blockStyle = styleFor(block, style);

        // Шейпинг переживает смену кегля и полосы: и то и другое меняет
        // только метрики, а они линейны по кеглю. Второй и все следующие
        // проходы обходятся одной разбивкой на строки.
        if (!shaped[index])
            shaped[index] = engine.shape(block.paragraph, blockStyle);

        item.lines = firstChar == 0
                         ? engine.layout(*shaped[index], measure, blockStyle)
                         : engine.layoutFrom(*shaped[index], firstChar, measure, blockStyle);

        // Заголовок держится за то, что за ним: сам по себе он ничего не
        // значит. Так же ведёт себя подзаголовок и подпись — она относится
        // к тому, что выше, но отрывать её тоже незачем.
        item.keepWithNext = block.kind == BlockKind::Title || block.kind == BlockKind::Subtitle;

        return item;
    }

    /* ---------------- порции ---------------- */

    void beginLayout(const PageStyle& pageStyle) {
        style = pageStyle;
        shaped.resize(blocks.size());

        laidOut.clear();
        laidOut.resize(blocks.size());
        layoutCursor = 0;
        lastNonEmpty = 0;
        complete = blocks.empty();

        book.reset(laidOut, style);
        if (complete)
            closeBook();
    }

    /// Считает без срока, пока `done` не скажет «хватит», — и до конца книги,
    /// если так и не скажет. Этим досчитывают книгу до места, куда читатель
    /// уже пошёл: порции хороши, пока он читает, а не ждёт ответа.
    /// @return false, когда книга досчитана.
    bool runUntil(const std::function<bool()>& done) {
        if (complete)
            return false;
        if (done())
            return true;   // уже досчитано досюда — лишнего блока не считаем

        while (layoutCursor < blocks.size()) {
            laidOut[layoutCursor] = layOut(layoutCursor, 0);
            if (laidOut[layoutCursor].occupies())
                lastNonEmpty = layoutCursor;
            ++layoutCursor;

            // Набор идёт за вёрсткой блок в блок, иначе условие остановки не
            // о чем спрашивать: страницы появляются именно здесь.
            book.place(lastNonEmpty);
            if (done())
                return true;
        }

        complete = true;
        book.place(blocks.size());
        closeBook();
        return false;
    }

    /// Считает, пока есть что считать и пока не вышел срок. Отсутствие срока —
    /// это «до конца книги»: так верстает `setStyle`, которому ждать некого.
    /// @return false, когда книга досчитана.
    bool run(const std::optional<std::chrono::steady_clock::time_point>& deadline) {
        if (complete)
            return false;

        while (layoutCursor < blocks.size()) {
            laidOut[layoutCursor] = layOut(layoutCursor, 0);
            if (laidOut[layoutCursor].occupies())
                lastNonEmpty = layoutCursor;
            ++layoutCursor;

            // Срок проверяется между блоками, а не внутри: разорвать вёрстку
            // абзаца нечем, да и незачем — самый дорогой блок из четырёх
            // тестовых книг стоит 27 мс.
            if (deadline && std::chrono::steady_clock::now() >= *deadline)
                break;
        }

        complete = layoutCursor == blocks.size();
        book.place(complete ? blocks.size() : lastNonEmpty);
        if (complete)
            closeBook();

        return !complete;
    }

    void closeBook() {
        book.finish();
        if (book.pages.empty())
            book.pages.push_back(Page{});
    }

    /* ---------------- мгновенная страница ---------------- */

    /// Блок, внутри которого лежит этот символ книги.
    std::size_t blockAt(std::uint32_t charOffset) const {
        const auto found = std::upper_bound(blocks.begin(), blocks.end(), charOffset,
                                            [](std::uint32_t offset, const Block& block) {
                                                return offset < block.charOffset;
                                            });
        if (found == blocks.begin())
            return 0;
        return static_cast<std::size_t>(std::distance(blocks.begin(), found) - 1);
    }

    /// Тот же символ, но в координатах текста абзаца.
    std::uint32_t charInBlock(std::size_t index, std::uint32_t charOffset) const {
        const std::vector<std::uint32_t>& offsets = blocks[index].paragraph.charOffsets;
        const auto found = std::lower_bound(offsets.begin(), offsets.end(), charOffset);
        if (found == offsets.end())
            return 0;   // место чтения дальше текста блока — начинаем с начала
        return static_cast<std::uint32_t>(std::distance(offsets.begin(), found));
    }

    void draftAt(const PageStyle& pageStyle, std::uint32_t charOffset, std::size_t count) {
        style = pageStyle;
        shaped.resize(blocks.size());

        draftBlocks.clear();
        draft.reset(draftBlocks, style);

        draftDone = blocks.empty();
        draftCursor = draftDone ? 0 : blockAt(charOffset);
        draftFirstChar = draftDone ? 0 : charInBlock(draftCursor, charOffset);

        draftUpTo(count);
    }

    /// Досчитывает грязную вёрстку до `count` страниц, продолжая с того блока,
    /// на котором остановилась.
    /// @return false, если книга кончилась раньше.
    bool draftUpTo(std::size_t count) {
        while (!draftDone && draft.pages.size() < count) {
            if (draftCursor >= blocks.size()) {
                // Книга кончилась — закрываем последнюю страницу.
                draft.finish();
                draftDone = true;
                break;
            }

            draftBlocks.push_back(layOut(draftCursor, draftFirstChar));
            draftFirstChar = 0;
            ++draftCursor;

            // Тот же уговор, что и у книги: набор не заходит за последний
            // непустой блок, потому что заглядывает вперёд.
            draft.place(nonEmptyLimit(draftBlocks));
        }

        return draft.pages.size() >= count;
    }
};

/* ================================================================== */

Paginator::Paginator(Engine& engine, std::span<const Block> blocks, std::uint32_t characterCount,
                     std::function<ImageSize(std::uint32_t)> imageSize)
    : impl_(std::make_unique<Impl>(engine, blocks, characterCount, std::move(imageSize))) {}

Paginator::~Paginator() = default;

void Paginator::setStyle(const PageStyle& style) {
    impl_->beginLayout(style);
    impl_->run(std::nullopt);
}

void Paginator::beginLayout(const PageStyle& style) { impl_->beginLayout(style); }

bool Paginator::advance(std::chrono::steady_clock::duration budget) {
    return impl_->run(std::chrono::steady_clock::now() + budget);
}

bool Paginator::advanceTo(std::uint32_t charOffset) {
    const std::vector<Page>& pages = impl_->book.pages;

    // Страница с этим символом окончательна, только когда набор ушёл за неё:
    // пока она последняя, на ней ещё будет место.
    return impl_->runUntil(
        [&] { return !pages.empty() && pages.back().firstCharOffset > charOffset; });
}

bool Paginator::advanceToPage(std::size_t index) {
    const std::vector<Page>& pages = impl_->book.pages;
    return impl_->runUntil([&] { return pages.size() > index; });
}

bool Paginator::isComplete() const { return impl_->complete; }

const PageStyle& Paginator::style() const { return impl_->style; }

std::size_t Paginator::pageCount() const { return impl_->book.pages.size(); }

namespace {

/// Пустая страница на случай, когда спросили, а страниц ещё нет. Отдать её
/// правильнее, чем прочесть за концом вектора: посреди порционной вёрстки
/// страниц может не быть ни одной, а спросить пагинатор может кто угодно.
const Page& nowhere() {
    static const Page empty;
    return empty;
}

}  // namespace

const Page& Paginator::page(std::size_t index) const {
    const std::vector<Page>& pages = impl_->book.pages;
    if (pages.empty())
        return nowhere();
    return pages[std::min(index, pages.size() - 1)];
}

std::size_t Paginator::draftCount() const { return impl_->draft.pages.size(); }

const Page& Paginator::draftPage(std::size_t index) const {
    const std::vector<Page>& pages = impl_->draft.pages;
    if (pages.empty())
        return nowhere();
    return pages[std::min(index, pages.size() - 1)];
}

void Paginator::draftAt(const PageStyle& style, std::uint32_t charOffset, std::size_t count) {
    impl_->draftAt(style, charOffset, count);
}

bool Paginator::draftUpTo(std::size_t count) { return impl_->draftUpTo(count); }

std::size_t Paginator::pageForCharOffset(std::uint32_t charOffset) const {
    const std::vector<Page>& pages = impl_->book.pages;

    // Страницы упорядочены по позиции в книге, поэтому — двоичный поиск
    // последней, начинающейся не позже искомого символа.
    const auto found = std::upper_bound(pages.begin(), pages.end(), charOffset,
                                        [](std::uint32_t offset, const Page& page) {
                                            return offset < page.firstCharOffset;
                                        });

    if (found == pages.begin())
        return 0;
    return static_cast<std::size_t>(std::distance(pages.begin(), found) - 1);
}

std::uint32_t Paginator::characterCount() const { return impl_->characterCount; }

std::span<const Block> Paginator::blocks() const { return impl_->blocks; }

}  // namespace bukvitsa::typography
