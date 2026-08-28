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
#include <memory>
#include <optional>
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
    /// @throw std::runtime_error, если файл не читается или это не FB3.
    Book(const std::filesystem::path& path, IDWriteFactory* dwrite);
    ~Book();

    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    const std::filesystem::path& path() const { return path_; }
    const fb3::Description& description() const { return document_.description(); }
    std::uint32_t characterCount() const { return document_.characterCount(); }

    /// Движок вёрстки книги. Отдан наружу затем, что сноску верстают теми же
    /// шрифтами и тем же кэшем, что и страницу.
    typography::Engine& engine() { return engine_; }

    typography::Paginator& paginator() { return *paginator_; }
    const typography::Paginator& paginator() const { return *paginator_; }

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

    std::filesystem::path path_;
    fb3::Document document_;
    typography::Engine engine_;
    std::unique_ptr<typography::Paginator> paginator_;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    std::vector<ImageAsset> images_;
};

}  // namespace bukvitsa::reader
