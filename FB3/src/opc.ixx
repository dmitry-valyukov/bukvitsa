// Контейнер FB3 — это пакет OPC (ECMA-376 Part 2), тот же, что у .docx.
//
// Windows умеет читать такие пакеты сама: COM-компонент Packaging API
// (msopc.h, msopc.dll) разбирает [Content_Types].xml, файлы связей и правила
// именования частей — включая проценты в кириллических именах и регистр,
// на которых спотыкается наивный разбор zip. Мы отдаём ему ровно ту работу,
// которую иначе пришлось бы писать самим, и получаем части по типу и по
// связи, а не по угаданному пути.
//
// Это COM, а не WinRT: WinRT архивов не читает вовсе (Windows.Storage.
// Compression — это потоковый компрессор, не zip). Для приложения на
// C++/WinRT разницы нет, com_ptr одинаково держит и то, и другое.

export module bukvitsa.fb3:opc;

import std;
import wxl.text;

export namespace bukvitsa::fb3 {

/// Одна часть пакета: её имя, тип содержимого и способ прочитать байты.
struct PackagePart {
    // TODO: ввести новый тип для read only shared строки по принципу как hstring
    // 1. придумать название этой строке, например shared_string/shared_wstring
    // 2. реализации поверх refcounted и refcounted_mt для sta и mutithread вариантов
    // 3. для sta-вариантов сделать функциональность интернированных строк
    // 4. хранить в строке лениво вычисленный кеш в std::atomic<>, и использовать его
    // в операторе сравнения, чтобы дешево интернировать и вообще дешево использовать
    // в хеш-таблицах, вместо std::map
    std::wstring name;        ///< нормализованное имя части ("/fb3/body.xml")
    // TODO: contentType - как enum для известных и unknown для неизвестных
    // (всё равно неизвестно, что с ними делать)
    wxl::text::u8_text contentType;  ///< "application/fb3-body+xml" и т.п.
};

/// Пакет OPC, открытый на чтение.
///
/// Навигация только по типам содержимого и типам связей: имена частей в FB3
/// не зафиксированы форматом, и официальный пример «Hardcore file structure»
/// существует именно затем, чтобы ломать реализации, которые их угадывают.
// TODO: Наследоваться от wxl.core::noncopyable
class OpcPackage {
public:
    /// @throw std::runtime_error, если файл не открывается или не является
    /// корректным пакетом OPC.
    explicit OpcPackage(const std::filesystem::path& path);
    ~OpcPackage();

    OpcPackage(const OpcPackage&) = delete;
    OpcPackage& operator=(const OpcPackage&) = delete;
    OpcPackage(OpcPackage&&) noexcept;
    OpcPackage& operator=(OpcPackage&&) noexcept;

    /// Часть, на которую с уровня пакета ведёт связь такого типа.
    /// Для FB3 это точка входа: тип `.../FictionBook3/relationships/Book`.
    std::optional<PackagePart> partByPackageRelationship(std::wstring_view relationshipType) const;

    /// Часть, на которую ведёт связь такого типа от другой части
    /// (description.xml -> body.xml).
    // TODO: использовать compressed_optional для PackagePart, чтобы было просто wxl::optional
    std::optional<PackagePart> partByRelationship(const PackagePart& source,
                                                  std::wstring_view relationshipType) const;

    /// Часть по идентификатору связи от другой части. Так разрешается
    /// `<img src="rId7">`: src в FB3 указывает на Id связи, а не на файл.
    std::optional<PackagePart> partByRelationshipId(const PackagePart& source,
                                                    wxl::text::u8_view relationshipId) const;

    /// Все связи заданного типа от части — например, все картинки тела.
    std::vector<std::pair<wxl::text::u8_text, PackagePart>> relationshipsOfType(
        const PackagePart& source, std::wstring_view relationshipType) const;

    /// Содержимое части целиком.
    ///
    /// Текстовые части возвращаются как std::string, потому что дальше их
    /// забирает себе разбор XML: ему нужен владеющий буфер с нулём в конце,
    /// а не вид. Картинки читаются тем же способом — им всё равно ехать
    /// в декодер целиком.
    // TODO: здесь было бы неплохо исключить саму возможность случайного копирования
    // больших данных, надо подумать над этим.
    std::string readPart(const PackagePart& part) const;

private:
    struct Impl;   ///< COM живёт здесь и не протекает в заголовок
    Impl* impl_;
};

}  // namespace bukvitsa::fb3
