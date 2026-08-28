#include "library.h"

#include <windows.h>



#include "book.h"
#include "settings.h"
#include "store.h"

// После своих заголовков: document.h тянет import wxl.core, а стандартный
// заголовок после импорта MSVC уже не принимает.
import wxl.text;
import wxl.xml;

namespace bukvitsa::reader {
namespace {

std::filesystem::path libraryPath() {
    return dataDirectory() / L"library.xml";
}

/// Расширение по типу содержимого части. Пустое значит «показать это нечем»:
/// обложку читает XAML, а он знает те же форматы, что и WIC.
std::wstring_view coverExtension(std::string_view contentType) {
    if (contentType == "image/jpeg" || contentType == "image/jpg") return L".jpg";
    if (contentType == "image/png") return L".png";
    if (contentType == "image/gif") return L".gif";
    if (contentType == "image/bmp") return L".bmp";
    return {};
}

std::filesystem::path statePath(std::wstring_view guid) {
    // Имя файла — guid и ничего больше: он наш, выдан CoCreateGuid, и в нём
    // не может оказаться ни разделителя пути, ни двоеточия. Названия книги
    // здесь нет намеренно — из него имя файла пришлось бы вычищать.
    return dataDirectory() / L"books" / (std::wstring{guid} + L".xml");
}

/// Один атрибут: имя, значение, экранирование. Отдельной функцией, потому что
/// в реестре их семь на запись, и повторять xmlValue() семь раз — значит однажды
/// забыть.
void attribute(wxl::text::text_builder<>& out, std::string_view name, std::wstring_view value) {
    out.format(" {}=\"{}\"", name, xmlValue(value));
}

/// Определены ниже, рядом с тем, что делают, — а нужны уже в add().
BookEntry describe(const Book& book);
std::wstring cacheCover(const Book& book, std::wstring_view guid);

}  // namespace

std::wstring newGuid() {
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) return {};

    wchar_t text[40]{};
    if (::StringFromGUID2(guid, text, static_cast<int>(std::size(text))) == 0) return {};
    return text;
}

void Library::load() {
    books_.clear();

    const std::filesystem::path path = libraryPath();
    std::error_code ignored;
    if (path.empty() || !std::filesystem::exists(path, ignored)) return;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load_file(path);

        for (const wxl::xml::node& element : root.children_named("book")) {
            BookEntry entry;
            entry.guid = attributeOf(element, "guid");
            entry.path = attributeOf(element, "path");
            entry.bookId = attributeOf(element, "bookId");
            entry.cover = attributeOf(element, "cover");
            entry.title = attributeOf(element, "title");
            entry.authors = attributeOf(element, "authors");
            entry.fileSize = numberOf(element, "size");
            entry.characterCount = static_cast<std::uint32_t>(numberOf(element, "characters"));

            // Без guid запись бесполезна: под ним лежит место чтения, и
            // выдать ей новый значило бы потерять прочитанное. Такого в файле,
            // который писали мы, не бывает — но файл могли и поправить руками.
            if (!entry.guid.empty() && !entry.path.empty()) books_.push_back(std::move(entry));
        }
    } catch (...) {
        // Битый реестр — это пустая витрина, а не отказ запуститься. Книги
        // при этом никуда не денутся: они лежат там, где лежали, и добавятся
        // снова.
        books_.clear();
    }
}

bool Library::save() const {
    wxl::text::text_builder<> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<library version=\"{}\">\n", kVersion);

    for (const BookEntry& entry : books_) {
        out.append("  <book");
        attribute(out, "guid", entry.guid);
        attribute(out, "title", entry.title);
        attribute(out, "authors", entry.authors);
        attribute(out, "bookId", entry.bookId);
        attribute(out, "cover", entry.cover);
        attribute(out, "path", entry.path);
        out.format(" size=\"{}\" characters=\"{}\"/>\n", entry.fileSize, entry.characterCount);
    }

    out.append("</library>\n");
    return writeFile(libraryPath(), out.view());
}

const BookEntry* Library::find(std::wstring_view guid) const {
    for (const BookEntry& entry : books_) {
        if (entry.guid == guid) return &entry;
    }
    return nullptr;
}

const BookEntry* Library::findSame(const BookEntry& candidate) const {
    if (!candidate.bookId.empty()) {
        for (const BookEntry& entry : books_) {
            if (entry.bookId == candidate.bookId) return &entry;
        }
    }

    // По пути — только если UUID не помог. Сравнение без учёта регистра:
    // на Windows это один и тот же файл.
    for (const BookEntry& entry : books_) {
        if (entry.path.size() == candidate.path.size() &&
            ::CompareStringOrdinal(entry.path.c_str(), static_cast<int>(entry.path.size()),
                                   candidate.path.c_str(), static_cast<int>(candidate.path.size()),
                                   TRUE) == CSTR_EQUAL) {
            return &entry;
        }
    }
    return nullptr;
}

const BookEntry& Library::add(const Book& book) {
    BookEntry entry = describe(book);

    if (const BookEntry* known = findSame(entry)) {
        // Guid остаётся прежним: за ним место чтения, и книга, которую
        // переложили в другую папку, должна открыться там же, где закрылась.
        BookEntry& stored = books_[static_cast<std::size_t>(known - books_.data())];
        entry.guid = stored.guid;
        entry.cover = cacheCover(book, entry.guid);
        stored = std::move(entry);
        return stored;
    }

    entry.guid = newGuid();
    entry.cover = cacheCover(book, entry.guid);
    books_.push_back(std::move(entry));
    return books_.back();
}

std::filesystem::path coverDirectory() {
    return dataDirectory() / L"cache";
}

namespace {

std::wstring cacheCover(const Book& book, std::wstring_view guid) {
    const std::optional<std::uint32_t> index = book.coverIndex();
    if (!index || guid.empty()) return {};

    const fb3::ImagePart* part = book.image(*index);
    if (!part || part->bytes.empty()) return {};

    const std::wstring_view extension = coverExtension(part->contentType.chars());
    if (extension.empty()) return {};   // svg и прочее, чего Image не покажет

    std::wstring name{guid};
    name += extension;
    if (!writeFile(coverDirectory() / name, part->bytes)) return {};
    return name;
}

BookEntry describe(const Book& book) {
    const fb3::Description& description = book.description();

    BookEntry entry;
    entry.path = book.path().wstring();
    // Всё это пришло из книги, а её документ wxl.xml проверила целиком, когда
    // открывала: assume_valid — запись этого довода в одном месте на три поля.
    // Ни одного assume_valid: модель книги отдаёт проверенный текст, потому
    // что документ проверила wxl.xml, когда его открывала.
    entry.bookId = description.id.to_utf16().wchars();
    entry.title = description.title.to_utf16().wchars();
    entry.authors = description.authorsLine().to_utf16().wchars();
    entry.characterCount = book.characterCount();

    std::error_code ignored;
    const auto size = std::filesystem::file_size(book.path(), ignored);
    if (!ignored) entry.fileSize = size;

    // Книга без названия бывает: в витрине лучше имя файла, чем пустая строка.
    if (entry.title.empty()) entry.title = book.path().filename().wstring();

    return entry;
}

}  // namespace

bool BookState::hasBookmark(std::uint32_t offset) const {
    for (const Bookmark& mark : bookmarks) {
        if (mark.charOffset == offset) return true;
    }
    return false;
}

BookState loadBookState(std::wstring_view guid) {
    BookState state;
    if (guid.empty()) return state;

    const std::filesystem::path path = statePath(guid);
    std::error_code ignored;
    if (!std::filesystem::exists(path, ignored)) return state;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load_file(path);

        if (const wxl::xml::node* reading = root.child("reading")) {
            state.charOffset = static_cast<std::uint32_t>(numberOf(*reading, "charOffset"));
        }
        if (const wxl::xml::node* marks = root.child("bookmarks")) {
            for (const wxl::xml::node& mark : marks->children_named("bookmark")) {
                Bookmark bookmark;
                bookmark.charOffset = static_cast<std::uint32_t>(numberOf(mark, "charOffset"));
                bookmark.hint = attributeOf(mark, "hint");
                state.bookmarks.push_back(std::move(bookmark));
            }
        }
    } catch (...) {
        // Битый файл состояния — это книга, открытая с начала, а не книга,
        // которая не открылась.
        return BookState{};
    }
    return state;
}

bool saveBookState(std::wstring_view guid, const BookState& state) {
    if (guid.empty()) return false;

    wxl::text::text_builder<> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<book version=\"{}\">\n", BookState::kVersion);
    // Позиция в символах книги, а не в страницах: страница меняется от кегля
    // и размера окна, символ — нет. Закладки меряются тем же.
    out.format("  <reading charOffset=\"{}\"/>\n", state.charOffset);

    if (!state.bookmarks.empty()) {
        out.append("  <bookmarks>\n");
        for (const Bookmark& mark : state.bookmarks) {
            out.format("    <bookmark charOffset=\"{}\" hint=\"{}\"/>\n", mark.charOffset,
                       xmlValue(mark.hint));
        }
        out.append("  </bookmarks>\n");
    }

    out.append("</book>\n");
    return writeFile(statePath(guid), out.view());
}

}  // namespace bukvitsa::reader
