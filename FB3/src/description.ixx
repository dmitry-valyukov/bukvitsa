// Метаданные книги: то, что читалка показывает в библиотеке, не открывая
// текст. В FB3 они лежат отдельной частью пакета — это и есть выигрыш
// формата перед FB2, где ради названия приходилось разбирать весь файл.

export module bukvitsa.fb3:description;

import std;
import wxl.text;

export namespace bukvitsa::fb3 {

/// Роль человека при книге. FB3 знает их около сорока; здесь те, что читалке
/// есть где показать, остальные попадают в Other с сохранённым именем роли.
enum class PersonRole : std::uint8_t {
    Author, Translator, Editor, Illustrator, Compiler,
    Narrator, Performer, Publisher, CopyrightHolder, Other,
};

// TODO: - интернированные строки везде, где только можно
struct Person {
    PersonRole role = PersonRole::Author;
    wxl::text::u8_text roleName;   ///< исходное имя роли, когда role == Other
    wxl::text::u8_text firstName;
    wxl::text::u8_text middleName;
    wxl::text::u8_text lastName;
    wxl::text::u8_text displayName; ///< как показывать одной строкой

    wxl::text::u8_text id;          ///< UUID автора: одно лицо в разных книгах
};

/// Место книги в серии; серии в FB3 вложенные («цикл» внутри «мира»).
struct SequenceEntry {
    wxl::text::u8_text name;
    // TODO: заменить на compressed optional где только можно
    std::optional<int> number;
};

/// Пробный фрагмент: сколько от книги здесь есть на самом деле.
struct FragmentInfo {
    std::uint64_t fullLength = 0;      ///< символов в полной книге
    std::uint64_t fragmentLength = 0;  ///< символов в этом файле
    bool isFragment() const { return fragmentLength != 0 && fragmentLength < fullLength; }
};

/// Разобранное description.xml.
struct Description {
    wxl::text::u8_text id;          ///< UUID книги
    wxl::text::u8_text version;

    wxl::text::u8_text title;
    wxl::text::u8_text subtitle;
    std::vector<wxl::text::u8_text> altTitles;

    std::vector<Person> persons;
    std::vector<SequenceEntry> sequences;

    wxl::text::u8_text language;
    wxl::text::u8_text annotation;   ///< простой текст: аннотация с разметкой живёт в теле
    wxl::text::u8_text publisher;
    std::optional<int> yearWritten;
    std::optional<int> yearPublished;
    std::vector<wxl::text::u8_text> isbns;
    std::vector<wxl::text::u8_text> subjects;   ///< тематика из fb3-classification

    std::optional<FragmentInfo> fragment;

    /// Обложка: индекс части в Document::images(), если книга её несёт.
    std::optional<std::uint32_t> coverImageIndex;

    /// Авторы одной строкой — то, что видно в списке книг.
    wxl::text::u8_text authorsLine() const;
};

}  // namespace bukvitsa::fb3
