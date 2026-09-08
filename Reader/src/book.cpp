#include <algorithm>
#include <iterator>
#include <stdexcept>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "book.h"

#include "bukvitsa/typography/block.h"

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;

IDWriteFactory* dwriteFactory() {
    // Живёт до конца процесса и намеренно не освобождается: разделяемую
    // фабрику DirectWrite всё равно держит система, а порядок разрушения
    // статиков на выходе нам не подконтролен.
    static IDWriteFactory* factory = nullptr;
    if (!factory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&factory));
    }
    return factory;
}

Book::Book(const std::filesystem::path& path, std::string fileBytes, IDWriteFactory* dwrite)
    : path_(path), document_(std::move(fileBytes)), engine_(dwrite) {
    decodeImages();

    // Пагинатор спрашивает размеры картинок у нас: вёрстка не декодирует
    // картинки и знать про WIC не должна.
    blocks_ = typography::flatten(document_.body());

    // Границы глав верхнего уровня: начало книги и каждый блок, открывающий
    // секцию верхнего уровня. По ним книга режется на главы.
    if (!blocks_.empty()) {
        chapterStarts_.push_back(0);
        for (std::size_t i = 1; i < blocks_.size(); ++i)
            if (blocks_[i].startsSection == 1)
                chapterStarts_.push_back(i);
    }

    // Главы заводятся по требованию (ensureChapter); размеры картинок каждой из
    // них нужны у нас — вёрстка их не декодирует и про WIC не знает.
    imageSize_ = [this](std::uint32_t index) {
        if (index >= images_.size())
            return typography::ImageSize{};
        return typography::ImageSize{images_[index].width, images_[index].height};
    };
}

Book::~Book() = default;

bool Book::setCurrentChapter(std::uint32_t charOffset) {
    if (chapterStarts_.empty())
        return false;

    // Блок, внутри которого лежит символ, — последний, начинающийся не позже.
    const auto blockIt = std::upper_bound(
        blocks_.begin(), blocks_.end(), charOffset,
        [](std::uint32_t off, const typography::Block& b) { return off < b.charOffset; });
    const std::size_t block =
        blockIt == blocks_.begin()
            ? 0
            : static_cast<std::size_t>(std::distance(blocks_.begin(), blockIt) - 1);

    // Глава — последнее её начало не позже этого блока.
    const auto chapterIt =
        std::upper_bound(chapterStarts_.begin(), chapterStarts_.end(), block);
    const std::size_t chapter =
        static_cast<std::size_t>(std::distance(chapterStarts_.begin(), chapterIt) - 1);

    if (chapter == currentChapter_)
        return false;

    currentChapter_ = chapter;
    // Заводим её в кэше (шейпинг соседних при этом не пропадает) и сбрасываем
    // раскладку под свежий стиль — переложит её читалка.
    ensureChapter(chapter).resetLayout();
    return true;
}

std::span<const typography::Block> Book::chapterSpan(std::size_t index) const {
    const std::size_t first = chapterStarts_[index];
    const std::size_t last =
        index + 1 < chapterStarts_.size() ? chapterStarts_[index + 1] : blocks_.size();
    return std::span<const typography::Block>(blocks_).subspan(first, last - first);
}

typography::Chapter& Book::ensureChapter(std::size_t index) {
    if (auto it = chapters_.find(index); it != chapters_.end())
        return *it->second;

    chapters_.emplace(index, std::make_unique<typography::Chapter>(
                                 engine_, chapterSpan(index), document_.characterCount(), imageSize_));

    // Кэш держим маленьким: горстка глав вокруг нужной. Лишние — самые дальние
    // от только что заведённой — выбрасываем; её саму и текущую оставляем.
    constexpr std::size_t kCacheSize = 4;
    while (chapters_.size() > kCacheSize) {
        auto worst = chapters_.end();
        std::size_t worstDist = 0;
        for (auto cand = chapters_.begin(); cand != chapters_.end(); ++cand) {
            if (cand->first == index || cand->first == currentChapter_)
                continue;
            const std::size_t dist =
                cand->first > index ? cand->first - index : index - cand->first;
            if (worst == chapters_.end() || dist > worstDist) {
                worstDist = dist;
                worst = cand;
            }
        }
        if (worst == chapters_.end())
            break;
        chapters_.erase(worst);
    }

    return *chapters_.at(index);
}

void Book::decodeImages() {
    const std::span<const fb3::ImagePart> parts = document_.images();
    images_.resize(parts.size());

    if (parts.empty())
        return;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic_))))
        return;   // без WIC книга читается, просто без иллюстраций

    for (std::uint32_t index = 0; index < parts.size(); ++index) {
        // Именно image(), а не parts[index]: байты части читаются при первом
        // обращении, и в списке они ещё пусты. Вёрстке нужны размеры всех
        // картинок сразу — без них не построить страницу, — так что лениться
        // тут не выходит.
        const fb3::ImagePart* part = document_.image(index);
        if (!part || part->bytes.empty())
            continue;

        // Размеры нужны пагинатору до всякой отрисовки, поэтому декодирование
        // идёт до конвертера, но не до видеокарты: битмап создаётся позже, на
        // том устройстве, которое будет рисовать.
        ComPtr<IWICStream> stream;
        if (FAILED(wic_->CreateStream(&stream))) continue;
        if (FAILED(stream->InitializeFromMemory(
                reinterpret_cast<BYTE*>(const_cast<char*>(part->bytes.data())),
                static_cast<DWORD>(part->bytes.size()))))
            continue;

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic_->CreateDecoderFromStream(stream.Get(), nullptr,
                                                 WICDecodeMetadataCacheOnLoad, &decoder)))
            continue;

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) continue;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wic_->CreateFormatConverter(&converter))) continue;
        if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut)))
            continue;

        UINT width = 0, height = 0;
        if (FAILED(frame->GetSize(&width, &height))) continue;

        images_[index].width = static_cast<float>(width);
        images_[index].height = static_cast<float>(height);
        images_[index].source = std::move(converter);
    }
}

const fb3::ImagePart* Book::image(std::uint32_t index) const {
    return document_.image(index);
}

std::optional<std::uint32_t> Book::coverIndex() const {
    return document_.description().coverImageIndex;
}

ID2D1Bitmap1* Book::bitmap(std::uint32_t index, ID2D1DeviceContext* context) {
    if (index >= images_.size())
        return nullptr;

    ImageAsset& asset = images_[index];
    if (asset.bitmap)
        return asset.bitmap.Get();
    if (!asset.source)
        return nullptr;

    if (FAILED(context->CreateBitmapFromWicBitmap(asset.source.Get(), nullptr, &asset.bitmap)))
        asset.source.Reset();   // не вышло — больше не пытаемся

    return asset.bitmap.Get();
}

}  // namespace bukvitsa::reader
