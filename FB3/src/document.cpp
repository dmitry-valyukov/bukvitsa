module bukvitsa.fb3;

import std;
import wxl.text;
import wxl.xml;

import :document;
import :internal;
import :node;
import :opc;
import :parse_helpers;

namespace bukvitsa::fb3 {

using xmlnode = wxl::xml::node;
using namespace detail;

namespace {

// Типы связей OPC, по которым устроена навигация. Имена частей формат не
// фиксирует, поэтому больше ориентироваться не на что.
constexpr std::wstring_view kRelBook = L"http://www.fictionbook.org/FictionBook3/relationships/Book";
constexpr std::wstring_view kRelBody = L"http://www.fictionbook.org/FictionBook3/relationships/body";
constexpr std::wstring_view kRelImage = L"http://www.fictionbook.org/FictionBook3/relationships/image";
constexpr std::wstring_view kRelThumbnail =
    L"http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail";

constexpr std::string_view kBodyNamespace = "http://www.fictionbook.org/FictionBook3/body";

/// Имя элемента FB3 -> вид узла. Виды, которых здесь нет, вливаются в
/// родителя вместе со своим содержимым: неизвестный элемент — это чаще
/// расширение формата, чем мусор, и терять из-за него текст книги нельзя.
std::optional<NodeKind> kindOf(std::string_view name) {
    static const std::unordered_map<std::string_view, NodeKind> table{
        {"section", NodeKind::Section},         {"title", NodeKind::Title},
        {"epigraph", NodeKind::Epigraph},       {"annotation", NodeKind::Annotation},
        {"subscription", NodeKind::Subscription}, {"notes", NodeKind::Notes},
        {"notebody", NodeKind::NoteBody},       {"p", NodeKind::Paragraph},
        {"subtitle", NodeKind::Subtitle},       {"pre", NodeKind::Preformatted},
        {"blockquote", NodeKind::Blockquote},   {"div", NodeKind::Div},
        {"ol", NodeKind::List},                 {"ul", NodeKind::List},
        {"li", NodeKind::ListItem},             {"poem", NodeKind::Poem},
        {"stanza", NodeKind::Stanza},           {"table", NodeKind::Table},
        {"tr", NodeKind::TableRow},             {"th", NodeKind::TableCell},
        {"td", NodeKind::TableCell},            {"br", NodeKind::Break},
        {"paper-page-break", NodeKind::PaperPageBreak}, {"marker", NodeKind::Marker},
        {"strong", NodeKind::Strong},           {"em", NodeKind::Emphasis},
        {"strikethrough", NodeKind::Strikethrough}, {"underline", NodeKind::Underline},
        {"sup", NodeKind::Superscript},         {"sub", NodeKind::Subscript},
        {"code", NodeKind::Code},               {"spacing", NodeKind::Spacing},
        {"smallcaps", NodeKind::SmallCaps},     {"span", NodeKind::Span},
        {"a", NodeKind::Link},                  {"note", NodeKind::NoteRef},
        {"img", NodeKind::Image},
    };

    const auto found = table.find(name);
    return found == table.end() ? std::nullopt : std::optional{found->second};
}

/// Держит ли этот вид прозу — то есть значим ли пробел внутри него.
///
/// Читатель XML отдаёт пробелы как есть, потому что не знает смысла
/// элементов; здесь смысл известен: между `</p>` и `<p>` перевод строки —
/// это отступ в файле, а между текстом и `<strong>` — пробел, который
/// увидит читающий книгу.
bool holdsInlineContent(NodeKind kind) {
    switch (kind) {
    case NodeKind::Paragraph:
    case NodeKind::Subtitle:
    case NodeKind::ListItem:
    case NodeKind::TableCell:
    case NodeKind::Preformatted:  // здесь значимо вообще всё, вплоть до переводов строк
        return true;
    default:
        return isInline(kind);
    }
}

}  // namespace

/* ------------------------------------------------------------------ */

struct Document::Impl {
    explicit Impl(const std::filesystem::path& path) : package(path) {}

    OpcPackage package;
    PackagePart descriptionPart;
    PackagePart bodyPart;

    // Оба — с текстовыми узлами (умолчание читателя): абзац книги это
    // смешанное содержимое, а метаданным хватает склейки кусков в textOf().
    wxl::xml::document bodyXml;
    wxl::xml::document descriptionXml;

    Description description;

    // Узлы и их нагрузка живут в deque: адреса не двигаются при росте,
    // а освобождается всё разом вместе с книгой.
    std::deque<Node> nodes;
    std::deque<SectionData> sections;
    std::deque<DivData> divs;
    std::deque<ImageData> imageRefs;
    std::deque<NoteRefData> noteRefs;
    std::deque<LinkData> links;
    std::deque<ListData> lists;
    std::deque<TableCellData> cells;
    std::deque<SpanData> spans;

    Node* body = nullptr;
    std::uint32_t characterCount = 0;

    std::unordered_map<std::string_view, const Node*> noteBodies;
    std::vector<NoteRefData*> pendingNoteRefs;

    mutable std::vector<ImagePart> images;
    std::unordered_map<std::string_view, std::uint32_t> imageIndex;

    /// Текстовые узлы в порядке документа: по ним восстанавливается место
    /// чтения из позиции символа — поиском по возрастающему ключу.
    std::vector<const Node*> textNodes;
};

/* ------------------------------------------------------------------ */

/// Превращает дерево читателя XML в дерево книги.
///
/// Обход рекурсивный, в отличие от самого читателя: книга вложена на
/// единицы уровней (секция в секции, врезка в абзаце), а не на тысячи,
/// и глубина всё равно проверяется — файл, который её превысил, скорее
/// испорчен, чем сложен.
class DocumentBuilder {
public:
    explicit DocumentBuilder(Document::Impl& document) : doc_(document) {}

    void build(const xmlnode& root) {
        doc_.body = makeNode(NodeKind::Body);
        buildChildren(root, *doc_.body);
        resolveNotes();
    }

private:
    static constexpr int kMaxDepth = 256;

    Node* makeNode(NodeKind kind, const void* data = nullptr) {
        Node& node = doc_.nodes.emplace_back();
        node.kind_ = kind;
        node.charOffset_ = doc_.characterCount;
        node.data_ = data;
        return &node;
    }

    static void append(Node& parent, Node& child) {
        child.parent_ = &parent;

        if (parent.firstChild_ == nullptr) {
            parent.firstChild_ = &child;
            return;
        }

        // Односвязный список: последний ребёнок ищется проходом. Дети одного
        // узла — это абзацы секции или прогоны абзаца, десятки, и хвост
        // в самом узле стоил бы памяти на каждом узле книги ради них.
        Node* last = parent.firstChild_;
        while (last->nextSibling_ != nullptr)
            last = last->nextSibling_;

        last->nextSibling_ = &child;
    }

    void buildChildren(const xmlnode& source, Node& parent, int depth = 0) {
        if (depth > kMaxDepth)
            throw std::runtime_error("FB3: слишком глубокая вложенность разметки");

        for (const xmlnode& child : source.children()) {
            if (child.type() == wxl::xml::node_type::text) {
                addText(child, parent);
                continue;
            }

            if (child.type() != wxl::xml::node_type::element)
                continue;

            // Отметка о вырезанном куске: не узел, а свойство секции.
            if (child.name() == "clipped") {
                if (parent.kind() == NodeKind::Section)
                    sectionDataOf(parent).clipped = true;
                continue;
            }

            const auto kind = kindOf(child.name().chars());

            if (!kind) {
                // Незнакомый элемент прозрачен: его содержимое остаётся
                // в книге, а сам он не мешает вёрстке.
                buildChildren(child, parent, depth + 1);
                continue;
            }

            Node* const node = makeNode(*kind, payloadFor(*kind, child));
            append(parent, *node);

            if (*kind == NodeKind::NoteBody)
                if (const auto id = child.attribute("id"))
                    doc_.noteBodies.emplace(id->chars(), node);

            buildChildren(child, *node, depth + 1);
        }
    }

    /// Нагрузку секции строил этот же обход, поэтому она своя, а не чужая.
    static SectionData& sectionDataOf(Node& node) {
        return *static_cast<SectionData*>(const_cast<void*>(node.data_));
    }

    void addText(const xmlnode& text, Node& parent) {
        const wxl::text::u8_view value = text.value();

        if (value.empty())
            return;

        // Отступы файла между блоками — не текст книги.
        if (!holdsInlineContent(parent.kind()) && wxl::text::trim(value.chars()).empty())
            return;

        Node* const node = makeNode(NodeKind::Text);
        node->text_ = value;
        append(parent, *node);

        doc_.textNodes.push_back(node);
        doc_.characterCount += static_cast<std::uint32_t>(wxl::text::count_code_points(value));
    }

    const void* payloadFor(NodeKind kind, const xmlnode& source) {
        switch (kind) {
        case NodeKind::Section: {
            SectionData& data = doc_.sections.emplace_back();
            data.id = source.attribute("id").value_or(wxl::text::u8_view{});
            data.doi = source.attribute("doi").value_or(wxl::text::u8_view{});
            data.output = toSectionOutput(source.attribute("output"));
            data.article = toBool(source.attribute("article"));
            data.clipped = source.child("clipped") != nullptr;
            data.firstCharPos = static_cast<std::uint32_t>(
                toInt(source.attribute("first-char-pos")).value_or(static_cast<int>(doc_.characterCount)));
            return &data;
        }

        case NodeKind::Div: {
            DivData& data = doc_.divs.emplace_back();
            data.floatMode = toFloatMode(source.attribute("float"));
            data.align = toAlign(source.attribute("align"));
            data.width = toLength(source.attribute("width"));
            data.minWidth = toLength(source.attribute("min-width"));
            data.maxWidth = toLength(source.attribute("max-width"));
            data.border = toBool(source.attribute("border"));
            data.keepOnOnePage = toBool(source.attribute("on-one-page"));
            data.bindTo = source.attribute("bindto").value_or(wxl::text::u8_view{});
            return &data;
        }

        case NodeKind::Image: {
            ImageData& data = doc_.imageRefs.emplace_back();
            data.relationshipId = source.attribute("src").value_or(wxl::text::u8_view{});
            data.alt = source.attribute("alt").value_or(wxl::text::u8_view{});
            data.width = toLength(source.attribute("width"));
            data.minWidth = toLength(source.attribute("min-width"));
            data.maxWidth = toLength(source.attribute("max-width"));

            if (const auto found = doc_.imageIndex.find(data.relationshipId.chars());
                found != doc_.imageIndex.end())
                data.partIndex = found->second;

            return &data;
        }

        case NodeKind::NoteRef: {
            NoteRefData& data = doc_.noteRefs.emplace_back();
            // Локальное имя: href у <note> голый, у <a> — в пространстве xlink,
            // и после разрешения имён это одно и то же "href".
            // "#id" внутри книги: решётка -- это разметка ссылки, а не часть
            // идентификатора, и срез по ней остаётся правильным текстом,
            // потому что режется он по ASCII-символу.
            std::string_view target = source.attribute("href").value_or(wxl::text::u8_view{}).chars();
            if (target.starts_with('#')) target.remove_prefix(1);
            data.targetId = wxl::text::assume_valid(target);
            data.role = toNoteRole(source.attribute("role"));
            data.numbering = toNoteNumbering(source.attribute("autotext"));
            doc_.pendingNoteRefs.push_back(&data);
            return &data;
        }

        case NodeKind::Link: {
            LinkData& data = doc_.links.emplace_back();
            data.href = source.attribute("href").value_or(wxl::text::u8_view{});
            data.internal = data.href.chars().starts_with('#');
            return &data;
        }

        case NodeKind::List: {
            ListData& data = doc_.lists.emplace_back();
            data.ordered = source.name() == "ol";
            if (const xmlnode* marker = source.child("marker"))
                if (const xmlnode* img = marker->child("img"))
                    data.markerImageRelationshipId = img->attribute("src").value_or(wxl::text::u8_view{});
            return &data;
        }

        case NodeKind::TableCell: {
            TableCellData& data = doc_.cells.emplace_back();
            data.header = source.name() == "th";
            data.colSpan = static_cast<std::uint16_t>(toInt(source.attribute("colspan")).value_or(1));
            data.rowSpan = static_cast<std::uint16_t>(toInt(source.attribute("rowspan")).value_or(1));
            data.align = toAlign(source.attribute("align"));
            data.verticalAlign = toVerticalAlign(source.attribute("valign"));
            return &data;
        }

        case NodeKind::Span: {
            SpanData& data = doc_.spans.emplace_back();
            data.className = source.attribute("class").value_or(wxl::text::u8_view{});
            return &data;
        }

        default:
            return nullptr;
        }
    }

    /// Ссылки на сноски разрешаются один раз, здесь: вёрстка перебирает
    /// книгу заново при каждой смене кегля, и искать по идентификатору
    /// на каждом проходе незачем.
    void resolveNotes() {
        for (NoteRefData* ref : doc_.pendingNoteRefs) {
            const auto found = doc_.noteBodies.find(ref->targetId.chars());
            if (found != doc_.noteBodies.end())
                ref->target = found->second;
        }
    }

    Document::Impl& doc_;
};

/* ------------------------------------------------------------------ */

Document::Document(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {
    // 1. Точка входа пакета: связь Book ведёт к метаданным, от них связь
    //    body — к тексту. Пути не угадываются нигде.
    auto description = impl_->package.partByPackageRelationship(kRelBook);
    if (!description)
        throw std::runtime_error("FB3: в пакете нет связи Book — это не книга FB3");

    impl_->descriptionPart = std::move(*description);

    auto body = impl_->package.partByRelationship(impl_->descriptionPart, kRelBody);
    if (!body)
        throw std::runtime_error("FB3: метаданные не ведут к телу книги");

    impl_->bodyPart = std::move(*body);

    // 2. Картинки известны до разбора тела: <img src> ссылается на Id связи,
    //    и узлу нужно сразу отдать индекс, а не идентификатор.
    auto imageRelationships = impl_->package.relationshipsOfType(impl_->bodyPart, kRelImage);
    impl_->images.reserve(imageRelationships.size());

    for (auto& [id, part] : imageRelationships) {
        ImagePart image;
        image.relationshipId = std::move(id);
        image.contentType = part.contentType;
        impl_->images.push_back(std::move(image));
    }

    // Указатели в список берутся, когда он перестал расти: иначе ключи
    // повисли бы на первом же его переезде.
    for (std::uint32_t i = 0; i < impl_->images.size(); ++i)
        impl_->imageIndex.emplace(impl_->images[i].relationshipId.chars(), i);

    // 3. Метаданные.
    const xmlnode& descriptionRoot =
        impl_->descriptionXml.load(impl_->package.readPart(impl_->descriptionPart), "description.xml");
    impl_->description = readDescription(descriptionRoot);

    // 4. Тело.
    const xmlnode& bodyRoot = impl_->bodyXml.load(impl_->package.readPart(impl_->bodyPart), "body.xml");

    // Пространство имён, а не запись: читатель XML их разрешает, и файл,
    // где издатель объявил для FB3 префикс, читается наравне с обычным.
    if (bodyRoot.namespace_uri() != kBodyNamespace)
        throw std::runtime_error(std::format(
            "FB3: тело книги записано в пространстве имён '{}', а не '{}'",
            bodyRoot.namespace_uri().chars(), kBodyNamespace));

    DocumentBuilder(*impl_).build(bodyRoot);

    // 5. Обложка: сначала штатная связь-миниатюра пакета, затем — картинка,
    //    названная обложкой. Второе эвристика, но именно так её кладёт
    //    редактор ЛитРес, а карточка книги без обложки заметно хуже.
    if (auto cover = impl_->package.partByPackageRelationship(kRelThumbnail)) {
        for (std::uint32_t i = 0; i < impl_->images.size(); ++i)
            if (impl_->images[i].contentType == cover->contentType)
                impl_->description.coverImageIndex = i;
    }

    if (!impl_->description.coverImageIndex) {
        for (std::uint32_t i = 0; i < impl_->images.size(); ++i) {
            std::string id(impl_->images[i].relationshipId.chars());
            wxl::text::make_ascii_lower(id);
            if (id.find("cover") != std::string::npos) {
                impl_->description.coverImageIndex = i;
                break;
            }
        }
    }
}

Document::~Document() = default;

const Description& Document::description() const { return impl_->description; }

const Node& Document::body() const { return *impl_->body; }

const Node* Document::noteBody(std::string_view id) const {
    const auto found = impl_->noteBodies.find(id);
    return found == impl_->noteBodies.end() ? nullptr : found->second;
}

std::uint32_t Document::characterCount() const { return impl_->characterCount; }

std::span<const ImagePart> Document::images() const { return impl_->images; }

const ImagePart* Document::image(std::uint32_t index) const {
    if (index >= impl_->images.size())
        return nullptr;

    ImagePart& part = impl_->images[index];

    // Байты читаются при первом обращении: книге с сотней иллюстраций
    // незачем держать их в памяти ради оглавления.
    if (part.bytes.empty())
        if (auto source = impl_->package.partByRelationshipId(impl_->bodyPart, part.relationshipId))
            part.bytes = impl_->package.readPart(*source);

    return &part;
}

const Node* Document::nodeAtCharOffset(std::uint32_t offset) const {
    const auto& texts = impl_->textNodes;

    // Позиции текстовых узлов возрастают по построению — поиск, а не обход.
    const auto found = std::ranges::upper_bound(texts, offset, {}, [](const Node* node) { return node->charOffset(); });

    if (found == texts.begin())
        return texts.empty() ? nullptr : texts.front();

    return *std::prev(found);
}

}  // namespace bukvitsa::fb3
