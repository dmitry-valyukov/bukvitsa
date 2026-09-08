#pragma once
// Открытая книга: документ, движок вёрстки, пагинатор и картинки.
//
// Всё, что живёт ровно столько же, сколько открытая книга, собрано в один
// объект — включая пагинатор, потому что он держит указатели в её узлы.
// Открыть другую книгу — значит построить новый Book и отпустить старый; так
// не остаётся ни одного места, где недействительный указатель мог бы пережить
// смену книги.
//
// Держать этот класс в общем модуле не за что: он ровно про то, что нужно
// приложению, и ни одному другому.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <span>
#include <vector>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

// После всех стандартных: заголовок вёрстки ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает -- значит и этот
// заголовок включается после всех стандартных.
#include "bukvitsa/typography/page.h"

import bukvitsa.fb3;

namespace bukvitsa::reader {

/// Общая фабрика DirectWrite: одна на процесс, как и положено разделяемой
/// фабрике. Её просят и книга (движок вёрстки), и полоса набора (колонцифра),
/// и заводить каждому свою значило бы держать два набора начертаний.
IDWriteFactory* dwriteFactory();

/// Картинка книги на пути от байтов к экрану.
struct ImageAsset {
    float width = 0.0f;    ///< в пикселях, как она лежит в файле
    float height = 0.0f;
    // TODO: разве в WinUI и WinRT нет соответствующих классов?
    // Ответ - есть, но они тяжеловесные.
    Microsoft::WRL::ComPtr<IWICFormatConverter> source;   ///< раскодированная, но ещё не на видеокарте
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;          ///< появляется при первой отрисовке
};

class Book {
public:
    /// Байты файла книга получает готовыми: читает их рабочий поток, потому
    /// что в интерфейсном обращений к диску не бывает, — а разбирает их
    /// конструктор, здесь, в интерфейсном (память разбора из STA-пула).
    /// Путь остаётся при книге как её имя: по нему она узнаётся в реестре и
    /// показывается читателю.
    ///
    /// @throw std::runtime_error, если это не FB3 или он повреждён.
    Book(const std::filesystem::path& path, std::string fileBytes, IDWriteFactory* dwrite);
    ~Book();

    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    const std::filesystem::path& path() const { return path_; }
    /// Разобранная книга. Отдана наружу затем, что реестру нужны её
    /// метаданные и обложка, а не открытая книга целиком.
    const fb3::Document& document() const { return document_; }

    const fb3::Description& description() const { return document_.description(); }
    std::uint32_t characterCount() const { return document_.characterCount(); }

    /// Движок вёрстки книги. Отдан наружу затем, что сноску верстают теми же
    /// шрифтами и тем же кэшем, что и страницу.
    typography::Engine& engine() { return engine_; }

    /// Размеченная текущая глава — из кэша, заведена по требованию и разложена
    /// читалкой под её стиль. Кэш держит ещё и соседние главы, чтобы у границы
    /// были под рукой обе; шейпинг каждой переживает переходы между ними.
    typography::Chapter& paginator() {
        return ensureChapter(currentChapter_ == static_cast<std::size_t>(-1) ? 0 : currentChapter_);
    }

    /// Книга, развёрнутая в блоки, — мастер-список для оглавления и поиска.
    /// Пагинатор смотрит в него же, но по одной главе за раз.
    std::span<const typography::Block> blocks() const { return blocks_; }

    /// Сколько в книге глав верхнего уровня — единиц, которыми она верстается.
    std::size_t chapterCount() const { return chapterStarts_.size(); }

    /// Глава, на которую пагинатор наведён сейчас.
    std::size_t currentChapter() const { return currentChapter_; }

    /// Символ, с которого начинается глава, — позиция её первого блока в книге.
    /// Индекс — из [0, chapterCount).
    std::uint32_t chapterFirstChar(std::size_t index) const {
        return blocks_[chapterStarts_[index]].charOffset;
    }

    /// Наводит пагинатор на главу, внутри которой лежит этот символ книги. Та
    /// же глава — ничего не делает, и её шейпинг не пропадает; другая —
    /// пагинатор сбрасывается на её блоки, и вёрстку главы надо начать заново.
    /// @return сменилась ли глава.
    bool setCurrentChapter(std::uint32_t charOffset);

    /// Часть-картинка книги как она лежит в пакете. Нужна тому, кто вынимает
    /// обложку: рисовать её незачем, надо положить байты в кэш.
    const fb3::ImagePart* image(std::uint32_t index) const;

    /// Индекс обложки в images(), если книга её несёт.
    std::optional<std::uint32_t> coverIndex() const;

    /// Картинка, готовая к рисованию. Создаётся при первом обращении: книга с
    /// иллюстрациями не должна занимать видеопамять ради оглавления.
    ///
    /// Битмап привязан к устройству контекста. Устройство у поверхностей
    /// композитора одно на процесс, так что пересоздавать его незачем — но
    /// если оно будет потеряно, эти битмапы придётся сбросить (долг записан
    /// в docs/reader.md).
    ID2D1Bitmap1* bitmap(std::uint32_t index, ID2D1DeviceContext* context);

private:
    void decodeImages();

    /// Заводит главу в кэше, если её там нет, и отдаёт. Дальние главы при этом
    /// выбрасываются — в памяти держится лишь горстка вокруг текущей.
    typography::Chapter& ensureChapter(std::size_t index);

    /// Блоки главы — вид в мастер-список.
    std::span<const typography::Block> chapterSpan(std::size_t index) const;

    std::filesystem::path path_;
    fb3::Document document_;
    typography::Engine engine_;

    /// Книга, развёрнутая в блоки, — мастер-список: на нём стоят оглавление и
    /// поиск, и в него же (видом, не копией) смотрят главы — каждая по своему
    /// куску. Заводится до глав и живёт дольше их: они держат вид в него.
    std::vector<typography::Block> blocks_;

    /// Размеры картинок по индексу — вёрстка их спрашивает у нас, а декодирует
    /// WIC. Хранится, чтобы отдавать каждой заводимой главе.
    std::function<typography::ImageSize(std::uint32_t)> imageSize_;

    /// Кэш размеченных глав: индекс → её объект. Держит текущую и соседние
    /// (дальние выбрасываются), чтобы на границе были обе главы разом, а шейпинг
    /// не считался заново при возврате к главе.
    std::map<std::size_t, std::unique_ptr<typography::Chapter>> chapters_;

    /// Индексы блоков — начала глав верхнего уровня; [0] всегда 0. Это границы
    /// глав в мастер-списке: по ним книга режется на главы.
    std::vector<std::size_t> chapterStarts_;

    /// Текущая глава — та, что показывают. npos — ещё ни одной: paginator()
    /// тогда заведёт нулевую, а первый setCurrentChapter наведёт на нужную.
    std::size_t currentChapter_ = static_cast<std::size_t>(-1);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    std::vector<ImageAsset> images_;
};

}  // namespace bukvitsa::reader
