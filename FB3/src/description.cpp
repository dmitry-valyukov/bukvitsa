module bukvitsa.fb3;

import std;
import wxl.text;
import wxl.xml;

import :description;
import :internal;
import :parse_helpers;

namespace bukvitsa::fb3 {
namespace {

using wxl::xml::node;

/// Весь текст поддерева одной строкой. Читатель XML отдаёт куски и не
/// склеивает их: склейка — дело того, кто знает, какая строка ему нужна.
/// Метаданным нужна владеющая копия, потому что `Description` переживает
/// документ, а кусок почти всегда ровно один — на этот случай копия делается
/// сразу нужного размера, без роста буфера.
wxl::text::u8_text textOf(const node* el) {
    if (!el) return {};

    const auto pieces = el->text_pieces();
    auto at = pieces.begin();

    if (at == pieces.end()) return {};

    const wxl::text::u8_view first = *at;

    if (++at == pieces.end()) return wxl::text::u8_text(first);

    std::string joined(first.chars());

    for (; at != pieces.end(); ++at)
        joined.append((*at).chars());

    // Куски резаны разметкой, а не посреди последовательности, поэтому склейка
    // правильного текста снова правильна -- вот и весь довод.
    return wxl::text::u8_text(wxl::text::assume_valid(joined));
}

PersonRole roleFromLink(wxl::text::u8_view link) {
    if (link == "author") return PersonRole::Author;
    if (link == "translator") return PersonRole::Translator;
    if (link == "editor") return PersonRole::Editor;
    if (link == "illustrator") return PersonRole::Illustrator;
    if (link == "compiler") return PersonRole::Compiler;
    if (link == "narrator") return PersonRole::Narrator;
    if (link == "performer") return PersonRole::Performer;
    if (link == "publisher") return PersonRole::Publisher;
    if (link == "copyright_holder") return PersonRole::CopyrightHolder;
    return PersonRole::Other;
}

/// Как показать человека одной строкой. Имя из частей собирается само —
/// но только если они есть: в реальных файлах у части людей заполнен лишь
/// `<title><main>`, и выдуманная из пустых частей строка была бы хуже него.
wxl::text::u8_text displayNameOf(const Person& person, wxl::text::u8_view titleMain) {
    std::string name;

    if (!person.firstName.empty()) name = person.firstName.chars();
    if (!person.middleName.empty()) {
        if (!name.empty()) name += ' ';
        name += person.middleName.chars();
    }
    if (!person.lastName.empty()) {
        if (!name.empty()) name += ' ';
        name += person.lastName.chars();
    }

    if (name.empty()) return wxl::text::u8_text(titleMain);

    return wxl::text::u8_text(wxl::text::assume_valid(name));
}

void readPersons(const node& relations, Description& description) {
    for (const node& subject : relations.children()) {
        // <object link='alt_media'> — это другие издания той же книги,
        // а не люди: у них своя роль в формате и нечего показывать в карточке.
        if (subject.name() != "subject")
            continue;

        Person person;
        // Отдельной константой, а не литералом прямо в value_or: проверка
        // литерала происходит в compile-time, а value_or -- обычная функция,
        // и передать через неё consteval-конструктор нельзя.
        static constexpr wxl::text::u8_view defaultLink = u8"author";

        const auto link = subject.attribute("link").value_or(defaultLink);
        person.role = roleFromLink(link);
        if (person.role == PersonRole::Other)
            person.roleName = wxl::text::u8_text(link);
        person.id = wxl::text::u8_text(subject.attribute("id").value_or(wxl::text::u8_view{}));

        person.firstName = textOf(subject.child("first-name"));
        person.middleName = textOf(subject.child("middle-name"));
        person.lastName = textOf(subject.child("last-name"));

        wxl::text::u8_text titleMain;
        if (const node* title = subject.child("title"))
            titleMain = textOf(title->child("main"));

        person.displayName = displayNameOf(person, titleMain);

        if (!person.displayName.empty())
            description.persons.push_back(std::move(person));
    }
}

void readSequences(const node& parent, Description& description) {
    for (const node& sequence : parent.children()) {
        if (sequence.name() != "sequence")
            continue;

        SequenceEntry entry;
        entry.name = wxl::text::u8_text(sequence.attribute("name").value_or(wxl::text::u8_view{}));
        entry.number = detail::toInt(sequence.attribute("number"));

        if (!entry.name.empty())
            description.sequences.push_back(std::move(entry));

        readSequences(sequence, description);  // серии в FB3 вложенные
    }
}

std::optional<int> yearOf(const node* dateHolder) {
    if (!dateHolder) return std::nullopt;

    const node* const date = dateHolder->child("date");
    if (!date) return std::nullopt;

    // Атрибут value — это ISO-дата ('2009-01-01'), содержимое элемента —
    // то, как её написал издатель ('2009'). Год берём из атрибута: он
    // разбирается однозначно.
    // Первые четыре байта ISO-даты -- это год, и проверяются они заново, а не
    // объявляются правильными: разрез по байту мог бы прийтись на середину
    // последовательности, если в атрибуте вместо даты оказалось что угодно.
    if (const auto value = date->attribute("value"); value && value->size() >= 4)
        return detail::toInt(wxl::text::checked(value->chars().substr(0, 4)));

    return detail::toInt(textOf(date));
}

}  // namespace

wxl::text::u8_text Description::authorsLine() const {
    std::string line;

    for (const Person& person : persons) {
        if (person.role != PersonRole::Author)
            continue;
        if (!line.empty())
            line += ", ";
        line += person.displayName.chars();
    }

    return wxl::text::u8_text(wxl::text::assume_valid(line));
}

/// Разбор description.xml. Внутренняя точка входа: Document зовёт её,
/// отдав уже разобранное дерево.
Description readDescription(const wxl::xml::node& root) {
    Description description;

    description.id = wxl::text::u8_text(root.attribute("id").value_or(wxl::text::u8_view{}));
    description.version = wxl::text::u8_text(root.attribute("version").value_or(wxl::text::u8_view{}));

    if (const node* title = root.child("title")) {
        description.title = textOf(title->child("main"));
        description.subtitle = textOf(title->child("sub"));

        for (const node& alt : title->children())
            if (alt.name() == "alt")
                description.altTitles.push_back(textOf(&alt));
    }

    if (const node* relations = root.child("fb3-relations")) {
        readPersons(*relations, description);

        for (const Person& person : description.persons)
            if (person.role == PersonRole::Publisher && description.publisher.empty())
                description.publisher = person.displayName;
    }

    readSequences(root, description);

    description.language = textOf(root.child("lang"));
    description.yearWritten = yearOf(root.child("written"));

    if (const node* classification = root.child("fb3-classification"))
        for (const node& subject : classification->children())
            if (subject.name() == "subject")
                if (auto text = textOf(&subject); !text.empty())
                    description.subjects.push_back(std::move(text));

    if (const node* annotation = root.child("annotation"))
        description.annotation = textOf(annotation);

    if (const node* info = root.child("document-info"))
        for (const node& isbn : info->children())
            if (isbn.name() == "isbn")
                if (auto text = textOf(&isbn); !text.empty())
                    description.isbns.push_back(std::move(text));

    for (std::size_t i = 0; const node* paper = root.child("paper-publish-info", i); ++i) {
        for (const node& child : paper->children())
            if (child.name() == "isbn")
                if (auto text = textOf(&child); !text.empty())
                    description.isbns.push_back(std::move(text));

        if (!description.yearPublished)
            description.yearPublished = yearOf(paper);
    }

    if (const node* fragment = root.child("fb3-fragment")) {
        FragmentInfo info;
        info.fullLength = static_cast<std::uint64_t>(detail::toInt(fragment->attribute("full_length")).value_or(0));
        info.fragmentLength =
            static_cast<std::uint64_t>(detail::toInt(fragment->attribute("fragment_length")).value_or(0));
        description.fragment = info;
    }

    return description;
}

}  // namespace bukvitsa::fb3
