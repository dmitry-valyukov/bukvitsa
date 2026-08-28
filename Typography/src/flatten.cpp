// Дерево книги -> список блоков.
//
// Два дела делаются здесь и больше нигде: свёртка пробелов (потому что это
// первое место, где известно, что значат элементы) и перевод текста в UTF-16
// (потому что дальше всё делает DirectWrite). Заодно строится таблица
// соответствия «символ строки -> позиция в книге»: свёртка рвёт прямое
// соответствие, а закладка обязана указывать на книгу, а не на результат
// вёрстки.

#include <algorithm>

// Свой заголовок после всех стандартных: он ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/block.h"

import bukvitsa.fb3;
import wxl.text;

namespace bukvitsa::typography {
namespace fb3 = bukvitsa::fb3;

namespace {

constexpr bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// Знак сноски, который ставит вёрстка, когда его не поставил автор.
///
/// В FB3 `<note>` сплошь и рядом пуст: в нём только ссылка на тело, а чем её
/// обозначить — дело того, кто набирает. Официальный пример `nightmare_example`
/// именно такой, и без этого знака его сноски были бы недостижимы — щёлкать
/// не по чему.
std::wstring noteMarker(fb3::NoteNumbering numbering, std::uint32_t number) {
    switch (numbering) {
    case fb3::NoteNumbering::Asterisk:
        // Звёздочки растут числом, а не значением: *, **, ***. Дальше третьей
        // это нечитаемо, поэтому дальше — цифры.
        if (number <= 3)
            return std::wstring(number, L'*');
        break;

    case fb3::NoteNumbering::Alpha:
        if (number >= 1 && number <= 26)
            return std::wstring(1, static_cast<wchar_t>(L'a' + number - 1));
        break;

    case fb3::NoteNumbering::Roman: {
        static constexpr std::pair<std::uint32_t, const wchar_t*> kRoman[] = {
            {1000, L"m"}, {900, L"cm"}, {500, L"d"}, {400, L"cd"}, {100, L"c"}, {90, L"xc"},
            {50, L"l"},   {40, L"xl"},  {10, L"x"},  {9, L"ix"},   {5, L"v"},   {4, L"iv"},
            {1, L"i"},
        };

        std::wstring result;
        std::uint32_t rest = number;
        for (const auto& [value, sign] : kRoman)
            while (rest >= value) {
                result += sign;
                rest -= value;
            }
        return result.empty() ? std::to_wstring(number) : result;
    }

    default:
        break;
    }

    // Арабские — и они же запасной вариант для всего, что не уложилось: сноска
    // без знака недостижима, а это хуже неточного знака.
    return std::to_wstring(number);
}

/// Есть ли среди прямых детей блочный узел. Определяет, разворачивать ли узел
/// как контейнер или как абзац: схема FB3 не даёт блок внутри `<p>`, но `<li>`
/// и ячейка таблицы бывают и с текстом, и с абзацами внутри.
bool hasBlockChild(const fb3::Node& node) {
    for (const fb3::Node& child : node.children())
        if (!fb3::isInline(child.kind()))
            return true;
    return false;
}

class Flattener {
public:
    std::vector<Block> run(const fb3::Node& body) {
        walk(body, Context{});
        finishBlock();
        return std::move(blocks_);
    }

private:
    /// Что наследуется вниз по дереву: во что превращать абзац, какого уровня
    /// заголовок, на какой глубине список.
    struct Context {
        BlockKind paragraphKind = BlockKind::Paragraph;
        std::uint8_t level = 0;
        std::uint8_t listDepth = 0;
        bool ordered = false;
    };

    std::vector<Block> blocks_;
    Block block_;
    bool open_ = false;
    bool preformatted_ = false;
    bool pendingSpace_ = false;      ///< свёрнутый пробел, ещё не записанный
    std::uint32_t pendingSpaceAt_ = 0;
    std::vector<FontStyle> styles_{FontStyle{}};

    /// Номер текущего блока. Нужен тем, кто уходит вглубь дерева и хочет
    /// вернуться к блоку, с которого начал: блок мог смениться под ними.
    std::uint64_t blockSerial_ = 0;

    /// Сквозной номер сноски по книге. Не по секции: секция в FB3 бывает
    /// вложена во что угодно, и «сноска 3» в двух местах книги смутила бы
    /// сильнее, чем «сноска 37».
    std::uint32_t noteNumber_ = 0;

    /* ---------------- блоки ---------------- */

    void beginBlock(BlockKind kind, const fb3::Node& source, const Context& context) {
        finishBlock();

        block_ = Block{};
        block_.kind = kind;
        block_.source = &source;
        block_.charOffset = source.charOffset();
        block_.level = context.level;
        block_.listDepth = context.listDepth;
        block_.ordered = context.ordered;

        open_ = true;
        preformatted_ = kind == BlockKind::Preformatted;
        pendingSpace_ = false;
        ++blockSerial_;
    }

    void finishBlock() {
        if (!open_) return;
        open_ = false;
        pendingSpace_ = false;

        // Хвостовой пробел абзаца не значит ничего: он появился от того, как
        // файл разложен по строкам.
        if (!preformatted_) {
            while (!block_.paragraph.text.empty() && block_.paragraph.text.back() == L' ') {
                block_.paragraph.text.pop_back();
                block_.paragraph.charOffsets.pop_back();
            }
        }

        trimSpans();

        if (!block_.paragraph.text.empty())
            blocks_.push_back(std::move(block_));

        block_ = Block{};
    }

    /// Обрезка хвостовых пробелов укоротила текст — прогоны надо подтянуть.
    void trimSpans() {
        const auto length = static_cast<std::uint32_t>(block_.paragraph.text.size());

        std::erase_if(block_.paragraph.spans, [&](const StyleSpan& span) { return span.start >= length; });
        for (StyleSpan& span : block_.paragraph.spans)
            span.length = std::min(span.length, length - span.start);

        std::erase_if(block_.paragraph.spans, [](const StyleSpan& span) { return span.length == 0; });

        std::erase_if(block_.paragraph.notes,
                      [&](const NoteAnchor& note) { return note.position >= length; });
        for (NoteAnchor& note : block_.paragraph.notes)
            note.length = std::min(note.length, length - note.position);

        // Знак нулевой длины ничего не занимает на строке, значит по нему
        // нельзя щёлкнуть, значит его нет.
        std::erase_if(block_.paragraph.notes,
                      [](const NoteAnchor& note) { return note.length == 0; });
    }

    /// Блок без текста — картинка или разделитель — пишется сразу.
    void emitStandalone(Block block) {
        finishBlock();
        blocks_.push_back(std::move(block));
    }

    /* ---------------- текст ---------------- */

    /// Прогоны стиля образуют разбиение текста, а не вложенные интервалы:
    /// новый прогон начинается там, где сменилось начертание. Так шейперу
    /// не приходится разбирать перекрытия, а `<strong><em>` — это просто
    /// прогон, у которого стоят оба флага.
    void appendUnit(wchar_t unit, std::uint32_t charOffset) {
        Paragraph& paragraph = block_.paragraph;

        if (paragraph.spans.empty() || !(paragraph.spans.back().style == styles_.back()))
            paragraph.spans.push_back(
                StyleSpan{static_cast<std::uint32_t>(paragraph.text.size()), 0, styles_.back()});

        block_.paragraph.text.push_back(unit);
        block_.paragraph.charOffsets.push_back(charOffset);
        ++block_.paragraph.spans.back().length;
    }

    void appendCodePoint(char32_t code, std::uint32_t charOffset) {
        if (code >= 0x10000u) {
            const char32_t rest = code - 0x10000u;
            appendUnit(static_cast<wchar_t>(0xD800u + (rest >> 10)), charOffset);
            appendUnit(static_cast<wchar_t>(0xDC00u + (rest & 0x3FFu)), charOffset);
        } else {
            appendUnit(static_cast<wchar_t>(code), charOffset);
        }
    }

    void flushPendingSpace() {
        if (!pendingSpace_) return;
        pendingSpace_ = false;

        // В начале абзаца свёрнутый пробел исчезает вовсе.
        if (block_.paragraph.text.empty())
            return;

        // И не удваивается: между двумя текстовыми узлами бывает инлайновый
        // элемент, который сам ничего не написал (пустая ссылка, знак сноски
        // без текста), — и тогда пробелы по обе стороны от него схлопываются
        // здесь, а не остаются двумя.
        const wchar_t last = block_.paragraph.text.back();
        if (last == L' ' || last == L'\n')
            return;

        appendUnit(L' ', pendingSpaceAt_);
    }

    void appendText(wxl::text::u8_view utf8, std::uint32_t firstCharOffset) {
        std::uint32_t offset = firstCharOffset;

        // Текст пришёл проверенным: wxl.xml проверяет документ целиком, прежде
        // чем его разбирать, и её дерево — а за ним и модель книги — отдаёт
        // u8_view. Обход кодовых точек берёт этот довод готовым и не
        // спрашивает заново на каждом байте.
        for (const char32_t code : wxl::text::code_points(utf8)) {
            if (!preformatted_ && code < 0x80u && isSpace(static_cast<char>(code))) {
                if (!pendingSpace_) {
                    pendingSpace_ = true;
                    pendingSpaceAt_ = offset;
                }
            } else {
                flushPendingSpace();
                appendCodePoint(code, offset);
            }

            ++offset;
        }
    }

    /* ---------------- стили ---------------- */

    void pushStyle(void (*apply)(FontStyle&)) {
        FontStyle style = styles_.back();
        apply(style);
        styles_.push_back(style);
    }

    void popStyle() { styles_.pop_back(); }

    /* ---------------- обход ---------------- */

    void walkChildren(const fb3::Node& node, Context context) {
        for (const fb3::Node& child : node.children())
            walk(child, context);
    }

    void inlineChildren(const fb3::Node& node) {
        Context inlineContext;
        for (const fb3::Node& child : node.children())
            walkInline(child, inlineContext);
    }

    void walkInline(const fb3::Node& node, const Context& context) {
        using fb3::NodeKind;

        switch (node.kind()) {
        case NodeKind::Text:
            appendText(node.text(), node.charOffset());
            return;

        case NodeKind::Strong:  pushStyle([](FontStyle& s) { s.bold = true; }); break;
        case NodeKind::Emphasis: pushStyle([](FontStyle& s) { s.italic = true; }); break;
        case NodeKind::Strikethrough: pushStyle([](FontStyle& s) { s.strikethrough = true; }); break;
        case NodeKind::Underline: pushStyle([](FontStyle& s) { s.underline = true; }); break;
        case NodeKind::Superscript: pushStyle([](FontStyle& s) { s.script = 1; }); break;
        case NodeKind::Subscript: pushStyle([](FontStyle& s) { s.script = -1; }); break;
        case NodeKind::Code: pushStyle([](FontStyle& s) { s.monospace = true; }); break;
        case NodeKind::SmallCaps: pushStyle([](FontStyle& s) { s.smallCaps = true; }); break;
        case NodeKind::Spacing: pushStyle([](FontStyle& s) { s.spaced = true; }); break;

        case NodeKind::NoteRef: {
            // Знак сноски набирается надстрочным, а место запоминается: по нему
            // читалка потом ловит щелчок и показывает тело сноски.
            flushPendingSpace();

            const auto anchor = block_.paragraph.notes.size();
            const std::uint64_t serial = blockSerial_;

            block_.paragraph.notes.push_back(
                NoteAnchor{static_cast<std::uint32_t>(block_.paragraph.text.size()), 0,
                           node.noteRef() ? node.noteRef()->target : nullptr});

            pushStyle([](FontStyle& s) { s.script = 1; });

            const auto before = block_.paragraph.text.size();
            for (const fb3::Node& child : node.children())
                walkInline(child, context);

            // Автор знака не поставил — ставим сами и нумеруем по порядку.
            if (serial == blockSerial_ && block_.paragraph.text.size() == before) {
                const fb3::NoteRefData* data = node.noteRef();
                const std::wstring marker = noteMarker(
                    data ? data->numbering : fb3::NoteNumbering::Arabic, ++noteNumber_);

                for (const wchar_t sign : marker)
                    appendUnit(sign, node.charOffset());
            }

            popStyle();

            // Длина знака известна только теперь: её набрали дети. Но пока они
            // набирались, блок мог смениться — картинка внутри знака сноски
            // закрывает абзац и начинает новый, — и тогда записывать длину
            // некуда: тот список сносок уже уехал вместе со старым блоком.
            if (serial == blockSerial_ && anchor < block_.paragraph.notes.size()) {
                NoteAnchor& note = block_.paragraph.notes[anchor];
                const auto end = static_cast<std::uint32_t>(block_.paragraph.text.size());
                note.length = end > note.position ? end - note.position : 0;
            }
            return;
        }

        case NodeKind::Image:
            // Картинка внутри абзаца разрывает его: v1 не умеет обтекания,
            // и честнее показать её отдельной строкой, чем потерять.
            emitImage(node);
            return;

        case NodeKind::Break:
            flushPendingSpace();
            appendUnit(L'\n', node.charOffset());
            return;

        default:
            // Ссылка, span и всё, чего вёрстка v1 не различает: содержимое
            // остаётся, оформление не меняется.
            for (const fb3::Node& child : node.children())
                walkInline(child, context);
            return;
        }

        for (const fb3::Node& child : node.children())
            walkInline(child, context);
        popStyle();
    }

    void emitImage(const fb3::Node& node) {
        Block image;
        image.kind = BlockKind::Image;
        image.source = &node;
        image.charOffset = node.charOffset();
        if (const fb3::ImageData* data = node.image())
            image.imageIndex = data->partIndex;

        // finishBlock() выбрасывает пустой абзац, поэтому картинка между двумя
        // кусками текста не оставляет за собой пустой строки. Запоминаются
        // только свойства блока: текст уезжает вместе с ним.
        const BlockKind kind = block_.kind;
        const fb3::Node* const source = block_.source;
        const std::uint8_t level = block_.level;
        const std::uint8_t listDepth = block_.listDepth;
        const bool ordered = block_.ordered;
        const bool wasOpen = open_;

        finishBlock();
        blocks_.push_back(std::move(image));

        if (wasOpen) {
            block_ = Block{};
            block_.kind = kind;
            block_.source = source;
            block_.charOffset = node.charOffset();
            block_.level = level;
            block_.listDepth = listDepth;
            block_.ordered = ordered;

            open_ = true;
            preformatted_ = kind == BlockKind::Preformatted;
            pendingSpace_ = false;
            ++blockSerial_;
        }
    }

    /// Узел, у которого нет блочных детей, целиком становится одним блоком.
    void emitParagraph(const fb3::Node& node, BlockKind kind, const Context& context) {
        beginBlock(kind, node, context);
        styles_.assign(1, FontStyle{});
        inlineChildren(node);
        finishBlock();
    }

    void walk(const fb3::Node& node, Context context) {
        using fb3::NodeKind;

        switch (node.kind()) {
        case NodeKind::Body:
            walkChildren(node, context);
            return;

        case NodeKind::Section:
            context.level = static_cast<std::uint8_t>(std::min(context.level + 1, 6));
            walkChildren(node, context);
            return;

        case NodeKind::Title:
            context.paragraphKind = BlockKind::Title;
            walkContainerOrParagraph(node, BlockKind::Title, context);
            return;

        case NodeKind::Epigraph:
            context.paragraphKind = BlockKind::Epigraph;
            walkChildren(node, context);
            return;

        case NodeKind::Annotation:
            context.paragraphKind = BlockKind::Annotation;
            walkChildren(node, context);
            return;

        case NodeKind::Subscription:
            context.paragraphKind = BlockKind::Subscription;
            walkContainerOrParagraph(node, BlockKind::Subscription, context);
            return;

        case NodeKind::Blockquote:
            context.paragraphKind = BlockKind::Quote;
            walkChildren(node, context);
            return;

        case NodeKind::Poem:
        case NodeKind::Stanza:
            context.paragraphKind = BlockKind::Verse;
            walkChildren(node, context);
            return;

        case NodeKind::List:
            context.listDepth = static_cast<std::uint8_t>(std::min(context.listDepth + 1, 6));
            context.ordered = node.list() && node.list()->ordered;
            context.paragraphKind = BlockKind::ListItem;
            walkChildren(node, context);
            return;

        case NodeKind::ListItem:
            walkContainerOrParagraph(node, BlockKind::ListItem, context);
            return;

        case NodeKind::Table:
        case NodeKind::TableRow:
            // v1 не верстает таблицы: ячейки идут подряд, как абзацы. Лучше
            // прочесть содержимое не в колонках, чем не прочесть вовсе.
            walkChildren(node, context);
            return;

        case NodeKind::TableCell:
            walkContainerOrParagraph(node, BlockKind::Paragraph, context);
            return;

        case NodeKind::Notes:
            // Блок сносок в поток чтения не идёт. Каждая сноска и так достижима
            // — по своему знаку, который стоит там, где на неё сослались, — а
            // вываленная в текст она читается как случайный кусок из другого
            // места. Тело сноски достаётся из книги по указателю в NoteAnchor.
            return;

        case NodeKind::NoteBody:
        case NodeKind::Div:
            walkChildren(node, context);
            return;

        case NodeKind::Paragraph:
            emitParagraph(node, context.paragraphKind, context);
            return;

        case NodeKind::Subtitle:
            emitParagraph(node, BlockKind::Subtitle, context);
            return;

        case NodeKind::Preformatted:
            emitParagraph(node, BlockKind::Preformatted, context);
            return;

        case NodeKind::Image:
            emitImage(node);
            return;

        case NodeKind::Break:
        case NodeKind::PaperPageBreak: {
            Block separator;
            separator.kind = BlockKind::Separator;
            separator.source = &node;
            separator.charOffset = node.charOffset();
            emitStandalone(std::move(separator));
            return;
        }

        case NodeKind::Marker:
            return;

        default:
            // Инлайновый узел там, где ждали блочный: бывает у книг, которые
            // писал не редактор, а скрипт. Заворачиваем в абзац.
            if (fb3::isInline(node.kind())) {
                emitParagraph(node, context.paragraphKind, context);
                return;
            }
            walkChildren(node, context);
            return;
        }
    }

    /// Узел вроде `<li>` или ячейки: бывает и с текстом, и с абзацами внутри.
    void walkContainerOrParagraph(const fb3::Node& node, BlockKind kind, Context context) {
        if (hasBlockChild(node)) {
            context.paragraphKind = kind;
            walkChildren(node, context);
        } else {
            emitParagraph(node, kind, context);
        }
    }
};

}  // namespace

std::vector<Block> flatten(const fb3::Node& body) {
    return Flattener{}.run(body);
}

}  // namespace bukvitsa::typography
