// Модель книги FB3: дерево узлов.
//
// Узлы — это то, что верстает Typography, поэтому дерево устроено под обход,
// а не под редактирование: один конкретный тип на все виды узлов (вид задан
// перечислением), дети связаны списком, атрибуты разобраны заранее в типизи-
// рованные структуры. Виртуальных функций нет: обход книги — это switch по
// kind(), а не диспетчеризация, и узел не платит за vptr, которых в книге
// десятки тысяч.
//
// Текст узлов — вид в буфер body.xml, которым владеет Document: разбор XML не
// копирует байты, и модель тоже. Отсюда единственное правило для пользователя:
// узлы живут ровно столько, сколько Document.
//
// Вид этот — `wxl::text::u8_view`, то есть текст, про который уже известно, что
// он правильный UTF-8: `wxl.xml` проверяет документ целиком, прежде чем его
// разбирать, и незачем спрашивать об этом заново у вёрстки на каждом абзаце.
// Сравнивается он с обычным литералом ровно так же, как раньше.

export module bukvitsa.fb3:node;

import std;
import wxl.text;

export namespace bukvitsa::fb3 {

/// Вид узла. Соответствует элементам fb3_body.xsd, но не один в один:
/// то, что для вёрстки одинаково, объединено (ol/ul -> List и флаг), а то,
/// что различается только оформлением, оставлено раздельным.
enum class NodeKind : std::uint8_t {
    // --- структура
    Body,
    Section,        ///< вложенная; у неё UUID, режим триала, первая позиция
    Title,
    Epigraph,
    Annotation,
    Subscription,   ///< подпись после текста
    Notes,          ///< блок сносок (подстрочных, концевых, комментариев)
    NoteBody,       ///< тело одной сноски, адресуется по id

    // --- блоки
    Paragraph,
    Subtitle,
    Preformatted,
    Blockquote,
    Div,            ///< единственный блок с настоящей геометрией (float, ширина)
    List,
    ListItem,
    Poem,
    Stanza,
    Table,
    TableRow,
    TableCell,
    Break,          ///< `<br clear="...">`
    PaperPageBreak, ///< граница страницы бумажного издания
    Marker,         ///< знак списка, заданный картинкой

    // --- инлайн
    Text,           ///< лист: кусок текста как он лежит в документе
    Strong,
    Emphasis,
    Strikethrough,
    Underline,
    Superscript,
    Subscript,
    Code,
    Spacing,        ///< разрядка
    SmallCaps,
    Span,           ///< произвольный class= — точка привязки стилей
    Link,           ///< `<a xlink:href>`
    NoteRef,        ///< ссылка на сноску; сам текст сноски лежит в NoteBody
    Image,          ///< `<img>` — и блочная, и инлайновая, решает контекст
};

/// Роль сноски: чем она станет при вёрстке. FB3 типизирует их сам, и это
/// подарок — подстрочную можно поставить внизу полосы, концевую увести
/// в конец, а комментарий показать по требованию.
enum class NoteRole : std::uint8_t { Footnote, Endnote, Comment, Auto, Other };

/// Как нумеровать сноску. `Keep` — не нумеровать: автор написал знак сам.
enum class NoteNumbering : std::uint8_t { Arabic, Roman, Alpha, Asterisk, Keep };

/// Как секция попадает в пробный фрагмент (атрибут output).
enum class SectionOutput : std::uint8_t { Default, Trial, TrialOnly, Payed };

enum class FloatMode : std::uint8_t { None, Left, Right, Center };
enum class Align : std::uint8_t { Inherit, Left, Right, Center, Justify };
enum class VerticalAlign : std::uint8_t { Inherit, Top, Middle, Bottom };

/// Размер, как его записывает FB3: число и единица. Пересчёт в DIP делает
/// Typography — только она знает кегль и метрики полосы, а модель обязана
/// сохранить написанное, иначе смена шрифта перестанет менять вёрстку.
struct Length {
    enum class Unit : std::uint8_t { None, Em, Ex, Percent, Millimeter };

    float value = 0.0f;
    Unit unit = Unit::None;

    bool isSet() const { return unit != Unit::None; }
};

/* ------------------------------------------------------------------ */
/* Полезная нагрузка. Лежит рядом с узлом в арене, а не внутри него:   */
/* её имеет меньшинство узлов, и Node не должен раздуваться до размера */
/* самого сложного из них.                                            */

struct SectionData {
    wxl::text::u8_view id;          ///< UUID секции — на нём стоят позиции чтения
    wxl::text::u8_view doi;
    SectionOutput output = SectionOutput::Default;
    bool article = false;         ///< самостоятельная единица (статья сборника)
    bool clipped = false;         ///< вырезана из пробного фрагмента
    std::uint32_t firstCharPos = 0; ///< позиция первого символа в полной книге
};

struct DivData {
    FloatMode floatMode = FloatMode::None;
    Align align = Align::Inherit;
    Length width, minWidth, maxWidth;
    bool border = false;
    bool keepOnOnePage = false;   ///< on-one-page: не разрывать между полосами
    wxl::text::u8_view bindTo;      ///< id элемента, к которому привязан плавающий блок
};

struct ImageData {
    wxl::text::u8_view relationshipId; ///< src= указывает на Id связи, не на файл
    wxl::text::u8_view alt;
    Length width, minWidth, maxWidth;
    std::uint32_t partIndex = 0;     ///< индекс части в Document::images()
};

struct NoteRefData {
    wxl::text::u8_view targetId;    ///< id соответствующего NoteBody
    NoteRole role = NoteRole::Auto;
    NoteNumbering numbering = NoteNumbering::Arabic;
    const class Node* target = nullptr; ///< разрешён при загрузке: без поиска при вёрстке
};

struct LinkData {
    wxl::text::u8_view href;        ///< внешний URL либо "#id" внутри книги
    bool internal = false;
};

struct ListData {
    bool ordered = false;         ///< ol против ul
    wxl::text::u8_view markerImageRelationshipId;
};

struct TableCellData {
    std::uint16_t colSpan = 1;
    std::uint16_t rowSpan = 1;
    Align align = Align::Inherit;
    VerticalAlign verticalAlign = VerticalAlign::Inherit;
    bool header = false;          ///< th против td
};

struct SpanData {
    wxl::text::u8_view className;
};

/* ------------------------------------------------------------------ */

/// Узел дерева книги.
///
/// Дети — односвязный список в порядке документа: книгу читают от начала
/// к концу, и обход назад нужен только вёрстке страницы вверх, которой
/// хватает родителя и заново пройденных детей.
class Node {
public:
    NodeKind kind() const { return kind_; }

    const Node* parent() const { return parent_; }
    const Node* firstChild() const { return firstChild_; }
    const Node* nextSibling() const { return nextSibling_; }
    bool hasChildren() const { return firstChild_ != nullptr; }

    /// Текст листа (kind() == Text). Вид в буфер документа, ничего не копирует.
    wxl::text::u8_view text() const { return text_; }

    /// Позиция первого символа узла в полной книге. Это валюта закладок
    /// и прогресса: она не зависит ни от кегля, ни от размера окна, ни от
    /// версии движка вёрстки — в отличие от номера страницы.
    std::uint32_t charOffset() const { return charOffset_; }

    /// Типизированные атрибуты. Возвращают nullptr, если вид узла другой —
    /// вызывающему не нужно помнить, у кого что бывает.
    /// @{
    const SectionData* section() const;
    const DivData* div() const;
    const ImageData* image() const;
    const NoteRefData* noteRef() const;
    const LinkData* link() const;
    const ListData* list() const;
    const TableCellData* tableCell() const;
    const SpanData* span() const;
    /// @}

    /// Обход детей в range-for: `for (const Node& child : node.children())`.
    class ChildRange {
    public:
        class iterator {
        public:
            explicit iterator(const Node* node = nullptr) : node_(node) {}
            const Node& operator*() const { return *node_; }
            iterator& operator++() { node_ = node_->nextSibling_; return *this; }
            bool operator!=(const iterator& other) const { return node_ != other.node_; }
        private:
            const Node* node_;
        };

        explicit ChildRange(const Node* first) : first_(first) {}
        iterator begin() const { return iterator(first_); }
        iterator end() const { return iterator(nullptr); }

    private:
        const Node* first_;
    };

    ChildRange children() const { return ChildRange(firstChild_); }

private:
    friend class DocumentBuilder;

    NodeKind kind_ = NodeKind::Text;
    std::uint32_t charOffset_ = 0;
    wxl::text::u8_view text_;
    const void* data_ = nullptr;   ///< указывает на структуру, которую называет kind_

    Node* parent_ = nullptr;
    Node* firstChild_ = nullptr;
    Node* nextSibling_ = nullptr;
};

/// Инлайновый ли это вид — то, что вливается в строку абзаца, а не образует
/// собственный блок. Вёрстка спрашивает об этом на каждом узле, поэтому это
/// сравнение диапазона, а не таблица.
constexpr bool isInline(NodeKind kind) {
    return kind >= NodeKind::Text && kind <= NodeKind::Image;
}

}  // namespace bukvitsa::fb3
