// Бэкенд MicroTeX поверх Direct2D и DirectWrite: три статические фабрики
// (tex::Font::create, tex::Font::_create, tex::TextLayout::create) и сам
// tex::Graphics2D. Дерево боксов рисуется прямыми командами в тот же
// ID2D1DeviceContext, которым рисуется страница, — тем же сглаживанием и тем
// же цветом темы; текст — DrawGlyphRun, как в glyph_painter.
//
// Упрощение, принятое сознательно: текст внутри формул (\text{...}) идёт по
// cmap шрифта без шейпинга и фолбэка — кириллице и латинице этого хватает, а
// сложное письмо внутри формулы в книгах не встречается. Понадобится —
// заменить на IDWriteTextLayout в одном месте, TextLayout_dw.

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

// MicroTeX — обычные заголовки, до нашего formula.h: тот ведёт к импорту
// модуля, после которого MSVC не примет ни одного нового стандартного
// заголовка, а MicroTeX их включает.
#include "graphic/graphic.h"
#include "graphic/graphic_basic.h"
#include "latex.h"
#include "render.h"
#include "utils/exceptions.h"

#include "bukvitsa/typography/formula.h"

using Microsoft::WRL::ComPtr;

namespace {

// Что фабрикам MicroTeX нужно от нас: фабрика DirectWrite и семейства для
// текстового режима. Статика, потому что сами фабрики — статические функции
// tex::Font; наполняет её FormulaEngine, единственный на приложение.
struct BackendState {
    ComPtr<IDWriteFactory> factory;
    std::wstring serif = L"Georgia";
    std::wstring sans = L"Segoe UI";
};

BackendState& backend() {
    static BackendState state;
    return state;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

}  // namespace

namespace tex {

namespace {

// Шрифт для MicroTeX: грань DirectWrite. Файловый (математические OTF из
// res) держит только face; именованный помнит семейство — от него
// производятся начертания deriveFont.
class Font_dw : public Font {
public:
    ComPtr<IDWriteFontFace> face;
    DWRITE_FONT_METRICS metrics{};
    std::wstring family;  // пусто у файлового
    float size = 0.0f;
    int style = TypefaceStyle::PLAIN;

    float getSize() const override { return size; }

    sptr<Font> deriveFont(int derived) const override;

    bool operator==(const Font& other) const override {
        const auto* rhs = static_cast<const Font_dw*>(&other);
        return face.Get() == rhs->face.Get() && size == rhs->size && style == rhs->style;
    }

    bool operator!=(const Font& other) const override { return !(*this == other); }

    // Ширина текста в DIP при кегле шрифта — по advance'ам дизайна.
    float measure(std::wstring_view text) const;

    // Подъём и свес в DIP.
    float ascent() const {
        return metrics.ascent * size / static_cast<float>(metrics.designUnitsPerEm);
    }
    float descent() const {
        return metrics.descent * size / static_cast<float>(metrics.designUnitsPerEm);
    }
};

using FontPtr = sptr<Font_dw>;

// Кодовые точки UTF-16 строки — GetGlyphIndices принимает их, а не единицы.
std::vector<UINT32> codepoints(std::wstring_view text) {
    std::vector<UINT32> points;
    points.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t unit = text[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size()) {
            const wchar_t low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                points.push_back(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                ++i;
                continue;
            }
        }
        points.push_back(unit);
    }
    return points;
}

std::vector<UINT16> glyphIndices(IDWriteFontFace* face, const std::vector<UINT32>& points) {
    std::vector<UINT16> indices(points.size());
    if (!points.empty()) {
        face->GetGlyphIndices(points.data(), static_cast<UINT32>(points.size()), indices.data());
    }
    return indices;
}

FontPtr createFileFont(const std::string& file, float size) {
    auto font = sptrOf<Font_dw>();
    font->size = size;

    const std::wstring path = widen(file);
    ComPtr<IDWriteFontFile> fontFile;
    if (FAILED(backend().factory->CreateFontFileReference(path.c_str(), nullptr, &fontFile))) {
        return font;
    }

    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType{};
    DWRITE_FONT_FACE_TYPE faceType{};
    UINT32 faces = 0;
    if (FAILED(fontFile->Analyze(&supported, &fileType, &faceType, &faces)) || !supported) {
        return font;
    }

    IDWriteFontFile* files[] = {fontFile.Get()};
    if (SUCCEEDED(backend().factory->CreateFontFace(faceType, 1, files, 0,
                                                    DWRITE_FONT_SIMULATIONS_NONE, &font->face))) {
        font->face->GetMetrics(&font->metrics);
    }
    return font;
}

FontPtr createNamedFont(std::wstring family, int style, float size) {
    auto font = sptrOf<Font_dw>();
    font->size = size;
    font->style = style;
    font->family = std::move(family);

    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(backend().factory->GetSystemFontCollection(&collection, FALSE))) return font;

    UINT32 index = 0;
    BOOL exists = FALSE;
    collection->FindFamilyName(font->family.c_str(), &index, &exists);
    if (!exists) index = 0;

    ComPtr<IDWriteFontFamily> fontFamily;
    if (FAILED(collection->GetFontFamily(index, &fontFamily))) return font;

    ComPtr<IDWriteFont> matched;
    const DWRITE_FONT_WEIGHT weight =
        (style & TypefaceStyle::BOLD) != 0 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    const DWRITE_FONT_STYLE slant =
        (style & TypefaceStyle::ITALIC) != 0 ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    if (FAILED(fontFamily->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, slant,
                                                &matched))) {
        return font;
    }

    if (SUCCEEDED(matched->CreateFontFace(&font->face))) {
        font->face->GetMetrics(&font->metrics);
    }
    return font;
}

}  // namespace

sptr<Font> Font_dw::deriveFont(int derived) const {
    if (family.empty()) {
        // Файловый шрифт: начертание в нём одно, им и остаёмся.
        auto copy = sptrOf<Font_dw>();
        copy->face = face;
        copy->metrics = metrics;
        copy->size = size;
        copy->style = derived;
        return copy;
    }
    return createNamedFont(family, derived, size);
}

float Font_dw::measure(std::wstring_view text) const {
    if (!face) return 0.0f;
    const std::vector<UINT32> points = codepoints(text);
    const std::vector<UINT16> indices = glyphIndices(face.Get(), points);
    std::vector<DWRITE_GLYPH_METRICS> glyphMetrics(indices.size());
    if (indices.empty() ||
        FAILED(face->GetDesignGlyphMetrics(indices.data(), static_cast<UINT32>(indices.size()),
                                           glyphMetrics.data(), FALSE))) {
        return 0.0f;
    }
    std::int64_t design = 0;
    for (const DWRITE_GLYPH_METRICS& glyph : glyphMetrics) design += glyph.advanceWidth;
    return static_cast<float>(design) * size / static_cast<float>(metrics.designUnitsPerEm);
}

// ---- статические фабрики, которые ядро MicroTeX ждёт от платформы ----

Font* Font::create(const std::string& file, float size) {
    // Владение отдаётся вызывающему — так объявлен контракт фабрики.
    auto font = createFileFont(file, size);
    return new Font_dw(*font);
}

sptr<Font> Font::_create(const std::string& name, int style, float size) {
    const BackendState& state = backend();
    std::wstring family;
    if (name.empty() || name == "Serif") family = state.serif;
    else if (name == "SansSerif") family = state.sans;
    else family = widen(name);
    return createNamedFont(std::move(family), style, size);
}

namespace {

class Graphics2D_d2d;

class TextLayout_dw : public TextLayout {
public:
    TextLayout_dw(std::wstring text, FontPtr font) : text_(std::move(text)), font_(std::move(font)) {}

    void getBounds(Rect& bounds) override {
        bounds.x = 0;
        bounds.y = -font_->ascent();
        bounds.w = font_->measure(text_);
        bounds.h = font_->ascent() + font_->descent();
    }

    void draw(Graphics2D& g2, float x, float y) override;

private:
    std::wstring text_;
    FontPtr font_;
};

}  // namespace

sptr<TextLayout> TextLayout::create(const std::wstring& src, const sptr<Font>& font) {
    return sptrOf<TextLayout_dw>(src, std::static_pointer_cast<Font_dw>(font));
}

namespace {

D2D1_COLOR_F toColorF(color argb) {
    return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(argb & 0xFF) / 255.0f,
                        static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
}

// Контекст рисования для MicroTeX: одна кисть, свой стек трансформаций
// поверх той, с которой пришёл контекст, восстановление в деструкторе.
class Graphics2D_d2d : public Graphics2D {
public:
    explicit Graphics2D_d2d(ID2D1DeviceContext* context) : context_(context) {
        context_->GetTransform(&base_);
        context_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush_);
        context_->GetFactory(&factory_);
    }

    // Не override: базовый Graphics2D деструктора не объявляет, а через
    // указатель на базу этот объект никто не удаляет.
    ~Graphics2D_d2d() { context_->SetTransform(base_); }

    void setColor(color c) override {
        color_ = c;
        if (brush_) brush_->SetColor(toColorF(c));
    }

    color getColor() const override { return color_; }

    void setStroke(const Stroke& stroke) override {
        stroke_ = stroke;
        strokeStyle_.Reset();  // пересоздастся лениво под новые cap/join
    }

    const Stroke& getStroke() const override { return stroke_; }

    void setStrokeWidth(float width) override { stroke_.lineWidth = width; }

    const Font* getFont() const override { return font_; }

    void setFont(const Font* font) override { font_ = font; }

    void translate(float dx, float dy) override {
        prepend(D2D1::Matrix3x2F::Translation(dx, dy));
    }

    void scale(float sx, float sy) override {
        prepend(D2D1::Matrix3x2F::Scale(sx, sy));
        sx_ *= sx;
        sy_ *= sy;
    }

    void rotate(float angle) override { rotate(angle, 0.0f, 0.0f); }

    void rotate(float angle, float px, float py) override {
        const float degrees = angle * 180.0f / 3.14159265358979323846f;
        prepend(D2D1::Matrix3x2F::Rotation(degrees, D2D1::Point2F(px, py)));
    }

    void reset() override {
        current_ = D2D1::Matrix3x2F::Identity();
        sx_ = sy_ = 1.0f;
        context_->SetTransform(base_);
    }

    float sx() const override { return sx_; }
    float sy() const override { return sy_; }

    void drawChar(wchar_t c, float x, float y) override {
        const wchar_t text[] = {c, L'\0'};
        drawTextRun(std::wstring_view(text, 1), x, y);
    }

    void drawText(const std::wstring& text, float x, float y) override {
        drawTextRun(text, x, y);
    }

    void drawLine(float x1, float y1, float x2, float y2) override {
        context_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(),
                           stroke_.lineWidth, strokeStyle());
    }

    void drawRect(float x, float y, float w, float h) override {
        context_->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), brush_.Get(),
                                stroke_.lineWidth, strokeStyle());
    }

    void fillRect(float x, float y, float w, float h) override {
        context_->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush_.Get());
    }

    void drawRoundRect(float x, float y, float w, float h, float rx, float ry) override {
        context_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry), brush_.Get(),
            stroke_.lineWidth, strokeStyle());
    }

    void fillRoundRect(float x, float y, float w, float h, float rx, float ry) override {
        context_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry), brush_.Get());
    }

    // Текст текущим (или явно данным) шрифтом, базовая линия в (x, y).
    // DrawGlyphRun без advance'ов — DirectWrite возьмёт дизайнерские, а
    // трансформация контекста отмасштабирует всё, включая глифы.
    void drawTextRun(std::wstring_view text, float x, float y,
                     const Font_dw* explicitFont = nullptr) {
        const auto* font = explicitFont ? explicitFont : static_cast<const Font_dw*>(font_);
        if (!font || !font->face) return;

        const std::vector<UINT32> points = codepoints(text);
        const std::vector<UINT16> indices = glyphIndices(font->face.Get(), points);
        if (indices.empty()) return;

        DWRITE_GLYPH_RUN run{};
        run.fontFace = font->face.Get();
        run.fontEmSize = font->size;
        run.glyphCount = static_cast<UINT32>(indices.size());
        run.glyphIndices = indices.data();

        context_->DrawGlyphRun(D2D1::Point2F(x, y), &run, brush_.Get());
    }

private:
    // Новая локальная операция происходит в текущих координатах, поэтому
    // встаёт перед накопленным: точка проходит сперва её, потом остальное.
    void prepend(const D2D1::Matrix3x2F& local) {
        current_ = local * current_;
        context_->SetTransform(current_ * base_);
    }

    ID2D1StrokeStyle* strokeStyle() {
        if (!strokeStyle_) {
            D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties();
            properties.startCap = properties.endCap = properties.dashCap = capOf(stroke_.cap);
            properties.lineJoin = joinOf(stroke_.join);
            properties.miterLimit = stroke_.miterLimit > 1.0f ? stroke_.miterLimit : 10.0f;
            factory_->CreateStrokeStyle(properties, nullptr, 0, &strokeStyle_);
        }
        return strokeStyle_.Get();
    }

    static D2D1_CAP_STYLE capOf(Cap cap) {
        switch (cap) {
        case CAP_BUTT: return D2D1_CAP_STYLE_FLAT;
        case CAP_SQUARE: return D2D1_CAP_STYLE_SQUARE;
        case CAP_ROUND: break;
        }
        return D2D1_CAP_STYLE_ROUND;
    }

    static D2D1_LINE_JOIN joinOf(Join join) {
        switch (join) {
        case JOIN_BEVEL: return D2D1_LINE_JOIN_BEVEL;
        case JOIN_MITER: return D2D1_LINE_JOIN_MITER;
        case JOIN_ROUND: break;
        }
        return D2D1_LINE_JOIN_ROUND;
    }

    ID2D1DeviceContext* context_ = nullptr;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> strokeStyle_;

    D2D1_MATRIX_3X2_F base_{};
    D2D1::Matrix3x2F current_ = D2D1::Matrix3x2F::Identity();
    float sx_ = 1.0f;
    float sy_ = 1.0f;

    color color_ = 0xFF000000;
    Stroke stroke_;
    const Font* font_ = nullptr;
};

}  // namespace

void TextLayout_dw::draw(Graphics2D& g2, float x, float y) {
    static_cast<Graphics2D_d2d&>(g2).drawTextRun(text_, x, y, font_.get());
}

}  // namespace tex

namespace bukvitsa::typography {

struct Formula::Impl {
    std::unique_ptr<tex::TeXRender> render;
};

Formula::Formula(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Formula::~Formula() = default;

float Formula::width() const { return static_cast<float>(impl_->render->getWidth()); }

float Formula::height() const { return static_cast<float>(impl_->render->getHeight()); }

float Formula::baseline() const {
    // MicroTeX отдаёт базовую линию долей высоты; вёрстке нужны DIP от верха.
    return impl_->render->getBaseline() * static_cast<float>(impl_->render->getHeight());
}

void Formula::draw(ID2D1DeviceContext* context, float x, float y) const {
    tex::Graphics2D_d2d graphics(context);
    impl_->render->draw(graphics, static_cast<int>(std::lround(x)),
                        static_cast<int>(std::lround(y)));
}

struct FormulaEngine::Impl {
    ComPtr<IDWriteFactory> factory;
};

FormulaEngine::FormulaEngine(IDWriteFactory* factory, const std::string& resourceRoot,
                             std::wstring serifFamily, std::wstring sansFamily)
    : impl_(std::make_unique<Impl>()) {
    impl_->factory = factory;

    BackendState& state = backend();
    state.factory = factory;
    state.serif = std::move(serifFamily);
    state.sans = std::move(sansFamily);

    // Контекст MicroTeX — статики; инициализация повторно не выполняется,
    // поэтому второй движок получил бы ресурсы первого. Один на приложение.
    static bool initialized = false;
    if (!initialized) {
        tex::LaTeX::init(resourceRoot);
        initialized = true;
    }
}

FormulaEngine::~FormulaEngine() {
    // LaTeX::release() намеренно не зовётся: он валит статический контекст,
    // а движок один и живёт до конца процесса — как Engine со шрифтами.
    backend().factory.Reset();
}

std::unique_ptr<Formula> FormulaEngine::parse(std::wstring_view tex, float textSize,
                                              float maxWidth, std::uint32_t argb) {
    backend().factory = impl_->factory;
    try {
        tex::TeXRender* render =
            tex::LaTeX::parse(std::wstring(tex), static_cast<int>(maxWidth), textSize,
                              textSize / 3.0f, argb);
        if (!render) return nullptr;
        auto impl = std::make_unique<Formula::Impl>();
        impl->render.reset(render);
        return std::unique_ptr<Formula>(new Formula(std::move(impl)));
    } catch (const tex::ex_tex&) {
        // Формула из чужой книги: большинство ошибок MicroTeX прощает сам и
        // рисует что понял, а что не простил — не повод падать; место
        // покажет исходный TeX, решает вызывающий. Ловится только его
        // исключение: bad_alloc и прочие беды программы идут наверх.
        return nullptr;
    }
}

}  // namespace bukvitsa::typography
