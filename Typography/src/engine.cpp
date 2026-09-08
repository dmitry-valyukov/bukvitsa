// Движок вёрстки: DirectWrite делает анализ и шейпинг, Кнут-Пласс — строки.
//
// Порядок работы над абзацем:
//   1. анализ всего абзаца сразу — письменности, направления, места переломов;
//   2. текст режется на прогоны формата: пересечение прогонов стиля,
//      письменности, направления и подобранного шрифта;
//   3. каждый прогон шейпится в глифы;
//   4. из ширин кластеров и мест переломов собираются боксы, клей и штрафы;
//   5. Кнут-Пласс выбирает переломы;
//   6. строки нарезаются из прогонов и выключаются.
//
// Анализ делается по абзацу целиком, а не по прогону: и BiDi, и правила
// переносов строки смотрят по сторонам от границы, и прогон, разобранный
// отдельно, дал бы другой ответ на своих краях.

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <map>
#include <span>
#include <tuple>
#include <unordered_map>

#include <dwrite_2.h>
#include <wrl/client.h>

// Свои заголовки после всех стандартных: они ведут к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/layout.h"
#include "bukvitsa/typography/linebreak.h"

namespace bukvitsa::typography {
namespace {

/// DirectWrite отвечает HRESULT, а вёрстка бросает исключения: обёртка, чтобы
/// каждый вызов не превращался в три строки.
inline void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error(std::format("DirectWrite: HRESULT 0x{:08X}", static_cast<unsigned>(hr)));
}
using Microsoft::WRL::ComPtr;

constexpr float kScriptScale = 0.66f;       ///< кегль над- и подстрочных
constexpr float kSuperscriptRise = 0.34f;   ///< доля кегля вверх
constexpr float kSubscriptDrop = 0.16f;
constexpr float kSpacedTracking = 0.22f;    ///< разрядка, доля кегля на глиф

/// Предел длины прогона шейпинга. Кластерная карта DirectWrite -- массив
/// UINT16, так что прогон, дающий больше 65535 глифов, разобрать нельзя;
/// запас взят с большим отрывом, потому что глифов бывает больше, чем
/// символов, и потому что короткие прогоны шейпятся заметно быстрее.
constexpr std::uint32_t kMaxRunLength = 4000;

/* ================================================================== */
/* Источник и приёмник анализа                                        */

/// Источник текста для анализаторов DirectWrite.
///
/// Живёт на стеке ровно на время анализа, поэтому счётчик ссылок здесь
/// формальность: DirectWrite не удерживает источник дольше вызова. Так же
/// сделано в образцах Microsoft (CustomLayout).
class AnalysisSource final : public IDWriteTextAnalysisSource {
public:
    AnalysisSource(const wchar_t* text, std::uint32_t length, const wchar_t* locale)
        : text_(text), length_(length), locale_(locale) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override {
        if (riid == __uuidof(IDWriteTextAnalysisSource) || riid == __uuidof(IUnknown)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, const WCHAR** text,
                                                UINT32* length) noexcept override {
        if (position >= length_) {
            *text = nullptr;
            *length = 0;
        } else {
            *text = text_ + position;
            *length = length_ - position;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, const WCHAR** text,
                                                    UINT32* length) noexcept override {
        if (position == 0 || position > length_) {
            *text = nullptr;
            *length = 0;
        } else {
            *text = text_;
            *length = position;
        }
        return S_OK;
    }

    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() noexcept override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* length,
                                            const WCHAR** locale) noexcept override {
        *length = length_ - std::min(position, length_);
        *locale = locale_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    GetNumberSubstitution(UINT32 position, UINT32* length,
                          IDWriteNumberSubstitution** substitution) noexcept override {
        *length = length_ - std::min(position, length_);
        *substitution = nullptr;
        return S_OK;
    }

private:
    const wchar_t* text_;
    std::uint32_t length_;
    const wchar_t* locale_;
};

/// Приёмник: складывает всё, что сказал анализ, в таблицы по символам.
/// По символу, а не списком интервалов, потому что дальше их читают вразнобой —
/// и при сборке прогонов формата, и при нарезке строк.
class AnalysisSink final : public IDWriteTextAnalysisSink {
public:
    explicit AnalysisSink(std::uint32_t length)
        : scripts_(length, DWRITE_SCRIPT_ANALYSIS{}), levels_(length, 0), breaks_(length) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override {
        if (riid == __uuidof(IDWriteTextAnalysisSink) || riid == __uuidof(IUnknown)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    HRESULT STDMETHODCALLTYPE
    SetScriptAnalysis(UINT32 position, UINT32 length,
                      const DWRITE_SCRIPT_ANALYSIS* analysis) noexcept override {
        for (UINT32 i = position; i < position + length && i < scripts_.size(); ++i)
            scripts_[i] = *analysis;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    SetLineBreakpoints(UINT32 position, UINT32 length,
                       const DWRITE_LINE_BREAKPOINT* points) noexcept override {
        for (UINT32 i = 0; i < length && position + i < breaks_.size(); ++i)
            breaks_[position + i] = points[i];
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32 position, UINT32 length, UINT8 /*explicitLevel*/,
                                           UINT8 resolvedLevel) noexcept override {
        for (UINT32 i = position; i < position + length && i < levels_.size(); ++i)
            levels_[i] = resolvedLevel;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32,
                                                    IDWriteNumberSubstitution*) noexcept override {
        return S_OK;
    }

    const pool_vector<DWRITE_SCRIPT_ANALYSIS>& scripts() const { return scripts_; }
    const pool_vector<std::uint8_t>& levels() const { return levels_; }
    const pool_vector<DWRITE_LINE_BREAKPOINT>& breakpoints() const { return breaks_; }

private:
    pool_vector<DWRITE_SCRIPT_ANALYSIS> scripts_;
    pool_vector<std::uint8_t> levels_;
    pool_vector<DWRITE_LINE_BREAKPOINT> breaks_;
};

/* ================================================================== */
/* Прогоны                                                            */

/// Прогон формата: кусок текста, у которого одинаковы стиль, письменность,
/// направление и шрифт. Это единица шейпинга.
struct FormatRun {
    std::uint32_t start = 0;
    std::uint32_t length = 0;
    FontStyle style;
    DWRITE_SCRIPT_ANALYSIS script{};
    std::uint8_t bidiLevel = 0;
    IDWriteFontFace* fontFace = nullptr;
    float fontSize = 0.0f;
    float ascenderOffset = 0.0f;   ///< для над- и подстрочных
};

/// Прогон после шейпинга. Кластерная карта остаётся с ним: без неё нельзя ни
/// нарезать прогон по границе строки, ни разложить выключку обратно.
struct ShapedRun {
    FormatRun format;
    pool_vector<std::uint16_t> glyphIndices;
    pool_vector<std::uint16_t> clusterMap;   ///< на символ: индекс первого глифа кластера
    pool_vector<float> advances;
    pool_vector<DWRITE_GLYPH_OFFSET> offsets;
    pool_vector<DWRITE_JUSTIFICATION_OPPORTUNITY> opportunities;
    float ascent = 0.0f;
    float descent = 0.0f;
};

/// Прогон, попавший в строку. Возможности выключки едут вместе с глифами:
/// они посчитаны по исходному тексту, которого у нарезанной строки уже нет.
struct LineRun {
    GlyphRun glyphs;
    pool_vector<DWRITE_JUSTIFICATION_OPPORTUNITY> opportunities;
};

/// Ключ кэша шрифтов: семейство плюс начертание.
struct FaceKey {
    std::wstring family;
    DWRITE_FONT_WEIGHT weight;
    DWRITE_FONT_STYLE style;

    bool operator<(const FaceKey& other) const {
        return std::tie(family, weight, style) < std::tie(other.family, other.weight, other.style);
    }
};

}  // namespace

/* ================================================================== */

/// Всё, что вёрстка узнаёт об абзаце один раз и переиспользует.
struct ShapedParagraph::Data {
    /// Абзац живёт у того, кто его завёл; здесь только вид в него. Строки
    /// ссылаются на его текст, поэтому пережить его нельзя (см. заголовок).
    const Paragraph* paragraph = nullptr;

    pool_vector<ShapedRun> runs;
    pool_vector<DWRITE_LINE_BREAKPOINT> breakpoints;

    /// Кегль, на котором посчитаны метрики прогонов.
    float referenceFontSize = 0.0f;

    /// То в стиле, что меняет сам шейпинг, а не его масштаб. Если при вёрстке
    /// это разойдётся — глифы уже не те, и абзац надо шейпить заново.
    bool bold = false;
    bool italic = false;
    bool monospace = false;
    std::uint64_t styleGeneration = 0;
};

ShapedParagraph::ShapedParagraph() : data_(std::make_unique<Data>()) {}
ShapedParagraph::~ShapedParagraph() = default;

float ShapedParagraph::referenceFontSize() const { return data_->referenceFontSize; }

/* ================================================================== */

struct Engine::Impl {
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteTextAnalyzer> analyzer;
    ComPtr<IDWriteTextAnalyzer1> analyzer1;      ///< выключка; может не быть
    ComPtr<IDWriteFontCollection> systemFonts;
    ComPtr<IDWriteFontFallback> fallback;        ///< подбор шрифта; может не быть

    TextStyle style;

    /// Растёт на каждую смену шрифта книги. Отшейпленный абзац помнит своё
    /// поколение, и по несовпадению видно, что глифы в нём уже не те, —
    /// иначе смена шрифта тихо не подействовала бы на всё уже отшейпленное.
    std::uint64_t styleGeneration = 0;

    std::map<FaceKey, ComPtr<IDWriteFontFace>> faces;
    std::unordered_map<IDWriteFontFace*, DWRITE_FONT_METRICS> metrics;
    /// Шрифты, найденные фолбэком. Ключ — сам указатель: DirectWrite отдаёт
    /// один и тот же объект на одно и то же начертание, поэтому карта не растёт
    /// от того, что фолбэк спрашивают на каждом абзаце.
    std::unordered_map<IDWriteFontFace*, ComPtr<IDWriteFontFace>> mapped;

    /* ---------------- шрифты ---------------- */

    /// Имя семейства -- нуль-терминированный указатель, а не вид: и
    /// FindFamilyName, и MapCharacters берут указатель без длины, а копию на
    /// каждый прогон формата платить не за что.
    IDWriteFontFace* baseFace(const wchar_t* family, bool bold, bool italic) {
        const FaceKey key{family, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                          italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL};

        if (const auto found = faces.find(key); found != faces.end())
            return found->second.Get();

        UINT32 index = 0;
        BOOL exists = FALSE;
        check(systemFonts->FindFamilyName(family, &index, &exists));

        // Шрифта нет — берём то, что есть у любой системы: молча падать из-за
        // настройки читателя нельзя.
        if (!exists)
            check(systemFonts->FindFamilyName(L"Segoe UI", &index, &exists));
        if (!exists)
            check(systemFonts->FindFamilyName(L"Arial", &index, &exists));

        ComPtr<IDWriteFontFamily> fontFamily;
        check(systemFonts->GetFontFamily(index, fontFamily.GetAddressOf()));

        ComPtr<IDWriteFont> font;
        check(
            fontFamily->GetFirstMatchingFont(key.weight, DWRITE_FONT_STRETCH_NORMAL, key.style, font.GetAddressOf()));

        ComPtr<IDWriteFontFace> face;
        check(font->CreateFontFace(face.GetAddressOf()));

        IDWriteFontFace* const raw = face.Get();
        faces.emplace(key, std::move(face));
        return raw;
    }

    const DWRITE_FONT_METRICS& metricsOf(IDWriteFontFace* face) {
        if (const auto found = metrics.find(face); found != metrics.end())
            return found->second;

        DWRITE_FONT_METRICS value{};
        face->GetMetrics(&value);
        return metrics.emplace(face, value).first->second;
    }

    const std::wstring& familyFor(const FontStyle& fontStyle) const {
        return fontStyle.monospace ? style.monospaceFamily : style.fontFamily;
    }

    float lineHeightOf(const ParagraphStyle& blockStyle) const {
        return blockStyle.fontSize * blockStyle.lineHeight;
    }

    /* ---------------- прогоны формата ---------------- */

    /// Режет прогон стиля на куски, каждому из которых нашёлся свой шрифт.
    /// Без фолбэка кусок один: базовый шрифт на всё.
    void appendFontRuns(pool_vector<FormatRun>& runs, const FormatRun& prototype, const wchar_t* text,
                        std::uint32_t textLength, const wchar_t* family, bool bold, bool italic) {
        const std::uint32_t end = prototype.start + prototype.length;

        const auto withBaseFont = [&](std::uint32_t from, std::uint32_t to) {
            FormatRun run = prototype;
            run.start = from;
            run.length = to - from;
            run.fontFace = baseFace(family, bold, italic);
            runs.push_back(run);
        };

        if (!fallback) {
            withBaseFont(prototype.start, end);
            return;
        }

        AnalysisSource source(text, textLength, style.locale.c_str());
        std::uint32_t at = prototype.start;

        while (at < end) {
            UINT32 mappedLength = 0;
            ComPtr<IDWriteFont> font;
            FLOAT scale = 1.0f;

            const HRESULT hr = fallback->MapCharacters(
                &source, at, end - at, systemFonts.Get(), const_cast<wchar_t*>(family),
                bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, &mappedLength, font.GetAddressOf(), &scale);

            if (FAILED(hr) || mappedLength == 0) {
                withBaseFont(at, end);
                return;
            }

            FormatRun run = prototype;
            run.start = at;
            // Ограничение обязательно: источник анализа отдаёт фолбэку текст до
            // конца абзаца, а не до конца прогона, и найденный кусок может
            // оказаться длиннее, чем мы спрашивали. Без этого прогоны налезли бы
            // друг на друга и глифы попали бы в строку дважды.
            run.length = std::min<std::uint32_t>(mappedLength, end - at);

            // Масштаб фолбэка выравнивает высоту строчных запасного шрифта по
            // основному. Применять его можно только когда шрифт действительно
            // найден: иначе MapCharacters оставляет в scale ноль, а кегль ноль
            // — это E_INVALIDARG на шейпинге.
            if (font && scale > 0.0f)
                run.fontSize = prototype.fontSize * scale;

            if (font) {
                ComPtr<IDWriteFontFace> face;
                if (SUCCEEDED(font->CreateFontFace(face.GetAddressOf()))) {
                    run.fontFace = face.Get();
                    mapped.emplace(face.Get(), face);
                }
            }
            if (!run.fontFace)
                run.fontFace = baseFace(family, bold, italic);

            runs.push_back(run);
            at += run.length;
        }
    }

    /// Пересечение всех разбиений сразу: стиль, письменность, направление, шрифт.
    pool_vector<FormatRun> formatRuns(const Paragraph& paragraph, const ParagraphStyle& blockStyle,
                                      const AnalysisSink& analysis) {
        const auto textLength = static_cast<std::uint32_t>(paragraph.text.size());
        pool_vector<FormatRun> runs;

        // Абзац без разметки — тот же абзац: один прогон стиля по умолчанию.
        // std::vector, а не пул: список подставляется вместо paragraph.spans
        // (модель книги, обычная куча), и в тернарном выборе типы обязаны
        // совпасть. Один элемент — цена пренебрежимая.
        const std::vector<StyleSpan> whole{StyleSpan{0, textLength, FontStyle{}}};
        const std::vector<StyleSpan>& spans = paragraph.spans.empty() ? whole : paragraph.spans;

        for (const StyleSpan& span : spans) {
            std::uint32_t at = span.start;
            const std::uint32_t spanEnd = std::min(span.start + span.length, textLength);

            while (at < spanEnd) {
                std::uint32_t end = at + 1;
                while (end < spanEnd && analysis.levels()[end] == analysis.levels()[at] &&
                       analysis.scripts()[end].script == analysis.scripts()[at].script &&
                       analysis.scripts()[end].shapes == analysis.scripts()[at].shapes)
                    ++end;

                // Кластерная карта DirectWrite шестнадцатибитная, поэтому
                // прогон длиннее её ёмкости шейпер отвергает. Делим по пробелу,
                // если он рядом: шейпинг через границу прогона теряет лигатуры,
                // и лучше потерять их на пробеле, чем в середине слова.
                if (end - at > kMaxRunLength) {
                    std::uint32_t cut = at + kMaxRunLength;
                    const std::uint32_t limit = cut - std::min<std::uint32_t>(kMaxRunLength / 8, cut - at - 1);
                    while (cut > limit && paragraph.text[cut - 1] != L' ')
                        --cut;
                    end = cut > at ? cut : at + kMaxRunLength;
                }

                // Начертание блока и начертание разметки складываются: курсив
                // внутри курсивного эпиграфа остаётся курсивом, а не спорит.
                FontStyle merged = span.style;
                merged.bold = merged.bold || blockStyle.bold;
                merged.italic = merged.italic || blockStyle.italic;
                merged.monospace = merged.monospace || blockStyle.monospace;

                FormatRun prototype;
                prototype.start = at;
                prototype.length = end - at;
                prototype.style = merged;
                prototype.script = analysis.scripts()[at];
                prototype.bidiLevel = analysis.levels()[at];
                prototype.fontSize = blockStyle.fontSize;

                if (merged.script > 0) {
                    prototype.fontSize *= kScriptScale;
                    prototype.ascenderOffset = blockStyle.fontSize * kSuperscriptRise;
                } else if (merged.script < 0) {
                    prototype.fontSize *= kScriptScale;
                    prototype.ascenderOffset = -blockStyle.fontSize * kSubscriptDrop;
                }

                appendFontRuns(runs, prototype, paragraph.text.c_str(), textLength, familyFor(merged).c_str(),
                               merged.bold, merged.italic);
                at = end;
            }
        }

        return runs;
    }

    /* ---------------- шейпинг ---------------- */

    ShapedRun shape(const Paragraph& paragraph, const FormatRun& format) {
        ShapedRun shaped;
        shaped.format = format;

        const wchar_t* const text = paragraph.text.c_str() + format.start;
        const UINT32 length = format.length;
        const BOOL rightToLeft = (format.bidiLevel & 1) != 0;

        // Маленькие прописные — единственная возможность OpenType, которую
        // формат просит явно (<smallcaps>); остальное шейпер решает сам.
        DWRITE_FONT_FEATURE smallCaps{DWRITE_FONT_FEATURE_TAG_SMALL_CAPITALS, 1};
        DWRITE_TYPOGRAPHIC_FEATURES features{&smallCaps, 1};
        const DWRITE_TYPOGRAPHIC_FEATURES* featureList = &features;
        const UINT32 featureRangeLength = length;

        const bool wantsFeatures = format.style.smallCaps;
        const DWRITE_TYPOGRAPHIC_FEATURES** featuresArgument = wantsFeatures ? &featureList : nullptr;
        const UINT32* rangesArgument = wantsFeatures ? &featureRangeLength : nullptr;
        const UINT32 rangeCount = wantsFeatures ? 1 : 0;

        shaped.clusterMap.resize(length);
        pool_vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProperties(length);
        pool_vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProperties;

        UINT32 maxGlyphs = 3 * length / 2 + 16;
        UINT32 actualGlyphs = 0;

        for (;;) {
            shaped.glyphIndices.resize(maxGlyphs);
            glyphProperties.resize(maxGlyphs);

            const HRESULT hr = analyzer->GetGlyphs(
                text, length, format.fontFace, FALSE, rightToLeft, &format.script,
                style.locale.c_str(), nullptr, featuresArgument, rangesArgument, rangeCount, maxGlyphs,
                shaped.clusterMap.data(), textProperties.data(), shaped.glyphIndices.data(),
                glyphProperties.data(), &actualGlyphs);

            if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
                maxGlyphs *= 2;
                continue;
            }
            check(hr);
            break;
        }

        shaped.glyphIndices.resize(actualGlyphs);
        glyphProperties.resize(actualGlyphs);
        shaped.advances.resize(actualGlyphs);
        shaped.offsets.resize(actualGlyphs);

        check(analyzer->GetGlyphPlacements(
            text, shaped.clusterMap.data(), textProperties.data(), length, shaped.glyphIndices.data(),
            glyphProperties.data(), actualGlyphs, format.fontFace, format.fontSize, FALSE, rightToLeft,
            &format.script, style.locale.c_str(), featuresArgument, rangesArgument, rangeCount,
            shaped.advances.data(), shaped.offsets.data()));

        // Возможности выключки спрашиваются здесь, пока на руках исходный текст
        // и свойства глифов: у нарезанной строки их уже не будет.
        shaped.opportunities.resize(actualGlyphs);
        if (analyzer1 && actualGlyphs > 0) {
            if (FAILED(analyzer1->GetJustificationOpportunities(
                    format.fontFace, format.fontSize, format.script, length, actualGlyphs, text,
                    shaped.clusterMap.data(), glyphProperties.data(), shaped.opportunities.data())))
                shaped.opportunities.assign(actualGlyphs, DWRITE_JUSTIFICATION_OPPORTUNITY{});
        }

        // Разрядка — не возможность шрифта, а типографский приём: раздвигаем
        // глифы сами. И вертикальный сдвиг над- и подстрочных.
        if (format.style.spaced)
            for (float& advance : shaped.advances)
                advance += format.fontSize * kSpacedTracking;

        if (format.ascenderOffset != 0.0f)
            for (DWRITE_GLYPH_OFFSET& offset : shaped.offsets)
                offset.ascenderOffset += format.ascenderOffset;

        const DWRITE_FONT_METRICS& fontMetrics = metricsOf(format.fontFace);
        const float scale = format.fontSize / static_cast<float>(fontMetrics.designUnitsPerEm);
        shaped.ascent = fontMetrics.ascent * scale;
        shaped.descent = fontMetrics.descent * scale;

        return shaped;
    }

    /// Анализ и шейпинг абзаца целиком — шаги 1–3 вёрстки. Дорогая часть:
    /// именно здесь происходят все обращения к DirectWrite, кроме выключки.
    void shapeInto(ShapedParagraph::Data& into, const Paragraph& paragraph,
                   const ParagraphStyle& blockStyle) {
        into.paragraph = &paragraph;
        into.referenceFontSize = blockStyle.fontSize;
        into.bold = blockStyle.bold;
        into.italic = blockStyle.italic;
        into.monospace = blockStyle.monospace;
        into.styleGeneration = styleGeneration;

        const auto textLength = static_cast<std::uint32_t>(paragraph.text.size());
        if (textLength == 0)
            return;

        AnalysisSource source(paragraph.text.c_str(), textLength, style.locale.c_str());
        AnalysisSink analysis(textLength);

        check(analyzer->AnalyzeScript(&source, 0, textLength, &analysis));
        check(analyzer->AnalyzeBidi(&source, 0, textLength, &analysis));
        check(analyzer->AnalyzeLineBreakpoints(&source, 0, textLength, &analysis));

        into.breakpoints = analysis.breakpoints();

        const pool_vector<FormatRun> formats = formatRuns(paragraph, blockStyle, analysis);
        into.runs.reserve(formats.size());

        for (const FormatRun& format : formats) {
            // Пустой прогон и прогон без шрифта — не ошибка вёрстки, а следствие
            // того, что разбиений четыре и они не обязаны совпадать. Шейперу такое
            // отдавать нельзя: он отвечает E_INVALIDARG.
            if (format.length == 0 || format.fontFace == nullptr || format.fontSize <= 0.0f)
                continue;
            into.runs.push_back(shape(paragraph, format));
        }
    }

    /* ---------------- ширины по символам ---------------- */

    /// Ширина каждого символа абзаца и то, насколько он готов сжаться.
    ///
    /// Ширину кластера несёт его первый символ, остальные получают ноль:
    /// разрывать кластер нельзя, и алгоритму строк достаточно знать, что внутри
    /// него ничего не стоит.
    ///
    /// Сжимаемость берётся не из головы, а из ответа DirectWrite: сколько он
    /// разрешил снять с этого глифа, столько Кнут-Пласс и вправе рассчитывать.
    /// Иначе алгоритм выбирает строку, надеясь сжать пробелы сильнее, чем
    /// выключка потом сумеет, — и строка вылезает за поле.
    struct CharacterMetrics {
        pool_vector<float> width;
        pool_vector<float> shrink;
    };

    static CharacterMetrics characterMetrics(const pool_vector<ShapedRun>& runs,
                                             std::uint32_t textLength, float scale) {
        CharacterMetrics metrics{pool_vector<float>(textLength, 0.0f),
                                 pool_vector<float>(textLength, 0.0f)};

        for (const ShapedRun& run : runs) {
            const std::uint32_t start = run.format.start;
            const std::uint32_t length = run.format.length;

            for (std::uint32_t i = 0; i < length; ++i) {
                const std::uint16_t glyph = run.clusterMap[i];
                if (i > 0 && run.clusterMap[i - 1] == glyph)
                    continue;

                std::uint32_t next = i + 1;
                while (next < length && run.clusterMap[next] == glyph)
                    ++next;

                const auto glyphEnd = next < length
                                          ? run.clusterMap[next]
                                          : static_cast<std::uint16_t>(run.glyphIndices.size());

                float width = 0.0f;
                float shrink = 0.0f;
                for (std::uint32_t g = glyph; g < glyphEnd && g < run.advances.size(); ++g) {
                    width += run.advances[g];
                    if (g < run.opportunities.size())
                        shrink += run.opportunities[g].compressionMaximum;
                }

                metrics.width[start + i] = width * scale;
                metrics.shrink[start + i] = shrink * scale;
            }
        }

        return metrics;
    }

    /* ---------------- боксы, клей и штрафы ---------------- */

    /// @param from символ, с которого начинается набор. Не ноль тогда, когда
    ///        страница начата с середины абзаца: мгновенная вёрстка ставит
    ///        первую строку ровно с места чтения, а не с красной строки.
    static pool_vector<BreakItem> breakItems(const Paragraph& paragraph,
                                             const CharacterMetrics& metrics,
                                             const pool_vector<DWRITE_LINE_BREAKPOINT>& points,
                                             bool ragged, float maxWidth, std::uint32_t from) {
        const auto textLength = static_cast<std::uint32_t>(paragraph.text.size());

        pool_vector<BreakItem> items;
        items.reserve((textLength - from) / 4 + 8);

        // Слово, которое не помещается в полосу целиком (длинный адрес,
        // склеенный текст, а в примере nightmare — нарочно целая строка
        // клавиатуры), приходится рубить по символу. Правила переносов такое
        // не разрывают и правы: разрыва там нет. Но показать книгу важнее
        // правила, которое здесь всё равно ничего не спасает.
        //
        // Рубится оно на четверти полосы, а не на полосу целиком: кусков на
        // строку тогда приходится несколько, и строка может кончиться там, где
        // надо, а не там, где случайно легла граница куска. Но проверяется
        // именно полная ширина слова — иначе под нож пошло бы каждое слово
        // длиннее четверти полосы, то есть половина слов в заголовке.
        const float chunkWidth = std::max(maxWidth * 0.25f, 1.0f);
        bool chopWord = false;

        float pending = 0.0f;
        float pendingShrink = 0.0f;
        bool pendingIsGlue = false;
        bool pendingStarted = false;
        std::uint32_t pendingStart = 0;

        const auto flush = [&](std::uint32_t endPosition) {
            if (!pendingStarted)
                return;
            pendingStarted = false;

            BreakItem item;
            item.textPosition = pendingStart;
            item.textEnd = endPosition;

            if (pendingIsGlue) {
                item.kind = BreakItem::Kind::Glue;
                item.width = pending;

                // Пробел тянется наполовину — значение plain TeX: оно задаёт ту
                // меру дыры, которую глаз ещё считает набором. Сжимаемость
                // берётся из ответа DirectWrite: сколько он разрешил снять,
                // столько разбивка и вправе рассчитывать.
                //
                // Свободный правый край делается не здесь, а через lineEndStretch
                // в настройках разбивки: клей на переломе съедается разрывом.
                item.stretch = pending * 0.5f;

                // Сжимаемость — меньшее из двух: сколько разрешил DirectWrite и
                // восьмая часть пробела. Второй предел нужен потому, что
                // GetJustificationOpportunities называет теоретический потолок,
                // а JustifyGlyphAdvances до него не дотягивает — он раздаёт
                // сжатие по приоритетам и останавливается раньше. Разбивка,
                // поверившая в потолок, выбирает строку, которую потом некому
                // сжать, и та вылезает за поле. Восьмая доля — то, что сжимается
                // всегда и незаметно для глаза.
                item.shrink = ragged ? 0.0f : std::min(pendingShrink, pending * 0.125f);
            } else {
                item.kind = BreakItem::Kind::Box;
                item.width = pending;
            }

            items.push_back(item);
            pending = 0.0f;
            pendingShrink = 0.0f;
        };

        const auto penalty = [&](std::uint32_t at, float value) {
            BreakItem item;
            item.kind = BreakItem::Kind::Penalty;
            item.penalty = value;
            item.textPosition = at;
            item.textEnd = at;
            items.push_back(item);
        };

        for (std::uint32_t i = from; i < textLength; ++i) {
            const DWRITE_LINE_BREAKPOINT& point = points[i];
            const bool space = point.isWhitespace != 0;

            if (i > from && point.breakConditionBefore == DWRITE_BREAK_CONDITION_MUST_BREAK) {
                flush(i);

                // Клей бесконечной растяжимости перед обязательным разрывом —
                // `\hfil\break` у Кнута. Без него строка, кончающаяся на <br>,
                // обязана заполнить полосу тем, что в ней есть, а она короткая
                // по смыслу; её коэффициент подгонки вылетает за терпимость,
                // разрыв признаётся невозможным — и, поскольку обязательный
                // разрыв закрывает разом все активные узлы, разбивка теряет
                // последний из них и не находит абзацу ни одного решения.
                BreakItem fill;
                fill.kind = BreakItem::Kind::Glue;
                fill.stretch = kInfinitePenalty;
                fill.textPosition = i;
                fill.textEnd = i;
                items.push_back(fill);

                penalty(i, -kInfinitePenalty);
            } else if (i > from && point.breakConditionBefore == DWRITE_BREAK_CONDITION_CAN_BREAK &&
                       !space && (points[i - 1].isWhitespace == 0)) {
                // Разрыв внутри непробельного текста: после дефиса, между
                // иероглифами. Штраф нулевой — место разрешено, но не желанно.
                flush(i);
                penalty(i, 0.0f);
            }

            if (!pendingStarted || space != pendingIsGlue) {
                flush(i);
                pendingStarted = true;
                pendingIsGlue = space;
                pendingStart = i;

                // Начинается слово — сразу смотрим, поместится ли оно в полосу
                // целиком. Просмотр вперёд проходит по каждому символу ровно
                // один раз за слово, то есть по книге — один раз.
                if (!space) {
                    float wordWidth = 0.0f;
                    for (std::uint32_t at = i; at < textLength && points[at].isWhitespace == 0; ++at)
                        wordWidth += metrics.width[at];
                    chopWord = wordWidth > maxWidth;
                }
            }

            // Слово шире всей полосы: длинный адрес, склеенный текст, а в
            // официальном примере nightmare — нарочно целая строка клавиатуры.
            // Правила переносов строки такое не разрывают и правы: разрыва там
            // нет. Но показать книгу важнее, чем соблюсти правило, которое
            // здесь всё равно ничего не спасает, поэтому рвём по символу — и
            // только здесь, когда иначе строка не поместится никак.
            if (chopWord && !pendingIsGlue && pending > 0.0f &&
                pending + metrics.width[i] > chunkWidth) {
                flush(i);

                // Клей нулевой ширины: место, где рвать можно, но ничего не
                // стоит. Кусков на строку выходит несколько, поэтому строка,
                // кончающаяся на таком клее, растяжимость всё же получает — от
                // тех же клеев, что остались внутри неё.
                BreakItem anywhere;
                anywhere.kind = BreakItem::Kind::Glue;
                anywhere.stretch = chunkWidth;
                anywhere.textPosition = i;
                anywhere.textEnd = i;
                items.push_back(anywhere);

                pendingStarted = true;
                pendingIsGlue = false;
                pendingStart = i;
            }

            pending += metrics.width[i];
            pendingShrink += metrics.shrink[i];
        }

        flush(textLength);

        // Хвост абзаца по Кнуту: бесконечно растяжимый клей и обязательный
        // разрыв. Так последняя строка не выключается по формату.
        BreakItem tail;
        tail.kind = BreakItem::Kind::Glue;
        tail.stretch = kInfinitePenalty;
        tail.textPosition = textLength;
        tail.textEnd = textLength;
        items.push_back(tail);

        penalty(textLength, -kInfinitePenalty);
        return items;
    }

    /* ---------------- нарезка строк ---------------- */

    /// Часть прогона, попавшая в строку. Кластерная карта указывает на глифы:
    /// чтобы взять символы [from, to), надо взять глифы, на которые они
    /// указывают, — и ни одним меньше, иначе развалится лигатура.
    /// @param scale отношение нужного кегля к опорному: метрики шейпинга
    ///        линейны по кеглю, поэтому строка при другом размере получается
    ///        умножением, а не новым обращением к DirectWrite. Умножается всё
    ///        сразу здесь, при нарезке, — копия глифов на строку всё равно
    ///        делается, а копировать книгу целиком ради масштаба незачем.
    static void sliceRun(const ShapedRun& run, std::uint32_t from, std::uint32_t to, LineRun& into,
                         float scale) {
        const std::uint32_t runStart = run.format.start;
        const std::uint32_t runEnd = runStart + run.format.length;

        if (to <= runStart || from >= runEnd)
            return;

        const std::uint32_t localFrom = std::max(from, runStart) - runStart;
        const std::uint32_t localTo = std::min(to, runEnd) - runStart;
        if (localFrom >= localTo)
            return;

        const std::uint16_t firstGlyph = run.clusterMap[localFrom];
        const auto lastGlyph = localTo < run.format.length
                                   ? run.clusterMap[localTo]
                                   : static_cast<std::uint16_t>(run.glyphIndices.size());

        into.glyphs.fontFace = run.format.fontFace;
        into.glyphs.fontSize = run.format.fontSize * scale;
        into.glyphs.bidiLevel = run.format.bidiLevel;
        into.glyphs.style = run.format.style;
        into.glyphs.textStart = runStart + localFrom;
        into.glyphs.textLength = localTo - localFrom;

        for (std::uint32_t g = firstGlyph; g < lastGlyph; ++g) {
            const float advance = run.advances[g] * scale;

            into.glyphs.glyphIndices.push_back(run.glyphIndices[g]);
            into.glyphs.advances.push_back(advance);

            DWRITE_GLYPH_OFFSET offset = run.offsets[g];
            offset.advanceOffset *= scale;
            offset.ascenderOffset *= scale;
            into.glyphs.offsets.push_back(offset);

            DWRITE_JUSTIFICATION_OPPORTUNITY opportunity =
                g < run.opportunities.size() ? run.opportunities[g]
                                             : DWRITE_JUSTIFICATION_OPPORTUNITY{};
            opportunity.expansionMinimum *= scale;
            opportunity.expansionMaximum *= scale;
            opportunity.compressionMaximum *= scale;
            into.opportunities.push_back(opportunity);

            into.glyphs.width += advance;
        }
    }

    /// Порядок прогонов на экране при смешанном направлении (правило L2 из
    /// алгоритма BiDi): последовательности уровня не ниже текущего
    /// переворачиваются, начиная с самого высокого уровня и вниз до самого
    /// низкого нечётного.
    static void reorderVisually(pool_vector<LineRun>& runs) {
        if (runs.size() < 2)
            return;

        std::uint8_t highest = 0;
        std::uint8_t lowestOdd = 63;
        for (const LineRun& run : runs) {
            highest = std::max(highest, run.glyphs.bidiLevel);
            if (run.glyphs.bidiLevel & 1)
                lowestOdd = std::min(lowestOdd, run.glyphs.bidiLevel);
        }

        for (int level = highest; level >= lowestOdd && level > 0; --level) {
            for (std::size_t i = 0; i < runs.size();) {
                if (runs[i].glyphs.bidiLevel < level) {
                    ++i;
                    continue;
                }
                std::size_t j = i;
                while (j < runs.size() && runs[j].glyphs.bidiLevel >= level)
                    ++j;
                std::reverse(runs.begin() + static_cast<std::ptrdiff_t>(i),
                             runs.begin() + static_cast<std::ptrdiff_t>(j));
                i = j;
            }
        }
    }

    /// Раздвигает строку до полосы. Ширины считает DirectWrite: он знает, где
    /// в этой письменности можно добавить, а где нельзя.
    void justify(pool_vector<LineRun>& runs, float available) {
        if (!analyzer1)
            return;

        pool_vector<float> advances;
        pool_vector<DWRITE_GLYPH_OFFSET> offsets;
        pool_vector<DWRITE_JUSTIFICATION_OPPORTUNITY> opportunities;

        for (const LineRun& run : runs) {
            advances.insert(advances.end(), run.glyphs.advances.begin(), run.glyphs.advances.end());
            offsets.insert(offsets.end(), run.glyphs.offsets.begin(), run.glyphs.offsets.end());
            opportunities.insert(opportunities.end(), run.opportunities.begin(), run.opportunities.end());
        }

        const auto glyphCount = static_cast<UINT32>(advances.size());
        if (glyphCount == 0)
            return;

        pool_vector<float> justified(glyphCount);
        pool_vector<DWRITE_GLYPH_OFFSET> justifiedOffsets(glyphCount);

        // Неудача здесь — не повод сдаться: строку всё равно надо уместить, и
        // ниже это доделает равномерное сжатие. Поэтому берём исходные ширины
        // и идём дальше, а не выходим.
        if (FAILED(analyzer1->JustifyGlyphAdvances(available, glyphCount, opportunities.data(),
                                                   advances.data(), offsets.data(), justified.data(),
                                                   justifiedOffsets.data()))) {
            justified = advances;
            justifiedOffsets = offsets;
        }

        // Остаток. DirectWrite сжимает ровно настолько, насколько разрешают
        // возможности, и на последних долях пикселя может не дотянуть. Строка,
        // вылезшая за поле, ломает всю страницу, поэтому такой остаток снимаем
        // равномерно — доля процента на межбуквенном просвете не видна.
        // Большой промах не трогаем: он означал бы ошибку в разбивке, и её
        // надо видеть, а не заглаживать.
        float total = 0.0f;
        for (const float advance : justified)
            total += advance;

        if (total > available && total <= available * 1.08f && total > 0.0f) {
            const float squeeze = available / total;
            for (float& advance : justified)
                advance *= squeeze;
        }

        UINT32 read = 0;
        for (LineRun& run : runs) {
            run.glyphs.width = 0.0f;
            for (std::size_t i = 0; i < run.glyphs.advances.size(); ++i, ++read) {
                run.glyphs.advances[i] = justified[read];
                run.glyphs.offsets[i] = justifiedOffsets[read];
                run.glyphs.width += justified[read];
            }
        }
    }
};

/* ================================================================== */

Engine::Engine(IDWriteFactory* factory) : impl_(std::make_unique<Impl>()) {
    impl_->factory = factory;
    check(factory->CreateTextAnalyzer(impl_->analyzer.GetAddressOf()));
    impl_->analyzer.As(&impl_->analyzer1);   // может не быть: см. justify()

    check(factory->GetSystemFontCollection(impl_->systemFonts.GetAddressOf(), FALSE));

    // Подбор запасного шрифта появился в IDWriteFactory2 (Windows 8.1). Без
    // него книга с редким символом покажет прямоугольник вместо глифа —
    // неприятно, но не смертельно, поэтому это не ошибка.
    ComPtr<IDWriteFactory2> factory2;
    if (SUCCEEDED(impl_->factory.As(&factory2)))
        factory2->GetSystemFontFallback(impl_->fallback.GetAddressOf());
}

Engine::~Engine() = default;

void Engine::setTextStyle(const TextStyle& style) {
    impl_->style = style;
    ++impl_->styleGeneration;
}

const TextStyle& Engine::textStyle() const { return impl_->style; }

float Engine::lineHeightFor(const ParagraphStyle& style) const { return impl_->lineHeightOf(style); }

ShapedParagraphPtr Engine::shape(const Paragraph& paragraph, const ParagraphStyle& style) {
    // Конструктор закрыт: отшейпленный абзац умеет делать только движок.
    ShapedParagraphPtr result(new ShapedParagraph());
    impl_->shapeInto(*result->data_, paragraph, style);
    return result;
}

pool_vector<Line> Engine::layout(const Paragraph& paragraph, float width,
                                 const ParagraphStyle& style) {
    const ShapedParagraphPtr shaped = shape(paragraph, style);
    return layout(*shaped, width, style);
}

pool_vector<Line> Engine::layout(const ShapedParagraph& given, float width,
                                 const ParagraphStyle& style) {
    return layoutRange(given, 0, width, style);
}

pool_vector<Line> Engine::layoutFrom(const ShapedParagraph& given, std::uint32_t firstChar,
                                     float width, const ParagraphStyle& style) {
    return layoutRange(given, firstChar, width, style);
}

pool_vector<Line> Engine::layoutRange(const ShapedParagraph& given, std::uint32_t firstChar,
                                      float width, const ParagraphStyle& style) {
    Impl& impl = *impl_;

    // Стиль мог разойтись с тем, на котором абзац шейпили, не только размером.
    // Тогда сохранённые глифы просто не те, и правильный ответ дороже быстрого:
    // шейпим заново на месте. Кэш зовущего от этого не чинится — но и не врёт.
    const ShapedParagraph::Data* data = given.data_.get();
    ShapedParagraphPtr reshaped;

    if (data->paragraph != nullptr &&
        (data->styleGeneration != impl.styleGeneration || data->bold != style.bold ||
         data->italic != style.italic || data->monospace != style.monospace)) {
        reshaped = shape(*data->paragraph, style);
        data = reshaped->data_.get();
    }

    const Paragraph& paragraph = *data->paragraph;
    const auto textLength = static_cast<std::uint32_t>(paragraph.text.size());
    if (textLength == 0 || firstChar >= textLength || width <= 0.0f ||
        data->referenceFontSize <= 0.0f)
        return {};

    /* 1-3. Анализ, прогоны формата и шейпинг уже сделаны — берём готовое.
       Кегль подгоняется отношением: метрики шейпинга линейны по нему. */
    const pool_vector<ShapedRun>& shaped = data->runs;
    const float scale = style.fontSize / data->referenceFontSize;

    /* 4-5. Переломы. */
    const bool ragged = style.alignment != Alignment::Justify;
    const Impl::CharacterMetrics metrics = Impl::characterMetrics(shaped, textLength, scale);
    const pool_vector<BreakItem> items =
        Impl::breakItems(paragraph, metrics, data->breakpoints, ragged, width, firstChar);

    // Отступ первой строки — свойство начала абзаца, а не начала полосы.
    // Страница, начатая с середины абзаца, его продолжает, и красная строка
    // посреди предложения соврала бы читателю о том, где он стоит.
    const float indent = firstChar == 0 ? style.firstLineIndent : 0.0f;
    const bool indented = indent != 0.0f;
    const float firstWidth = std::max(width - indent, width * 0.2f);
    const float lineWidths[2] = {firstWidth, width};

    BreakSettings settings;
    if (ragged) {
        // Свободный правый край: строка вправе кончиться где угодно, и выбор
        // переломов идёт по штрафам, а не по плотности набора.
        settings.lineEndStretch = kInfinitePenalty;
    }

    const std::vector<std::uint32_t> breaks =
        breakLines(items, std::span<const float>(lineWidths, indented ? 2 : 1), settings);

    if (breaks.empty())
        return {};

    /* 6. Строки. */
    pool_vector<Line> lines;
    lines.reserve(breaks.size());

    std::uint32_t from = firstChar;
    for (std::size_t index = 0; index < breaks.size(); ++index) {
        const BreakItem& at = items[breaks[index]];
        const std::uint32_t to = std::max(from, at.textPosition);

        pool_vector<LineRun> runs;
        Line line;
        line.textStart = from;
        line.textLength = to - from;
        line.charOffset = from < paragraph.charOffsets.size()
                              ? paragraph.charOffsets[from]
                              : (paragraph.charOffsets.empty() ? 0 : paragraph.charOffsets.back());

        for (const ShapedRun& run : shaped) {
            LineRun piece;
            Impl::sliceRun(run, from, to, piece, scale);
            if (piece.glyphs.glyphIndices.empty())
                continue;

            runs.push_back(std::move(piece));
            line.ascent = std::max(line.ascent, run.ascent * scale);
            line.descent = std::max(line.descent, run.descent * scale);
        }

        Impl::reorderVisually(runs);

        const float available = index == 0 ? firstWidth : width;
        const bool lastLine = index + 1 == breaks.size();

        float natural = 0.0f;
        for (const LineRun& run : runs)
            natural += run.glyphs.width;

        // Выключка идёт в обе стороны: Кнут-Пласс нарочно берёт строку шире
        // полосы, когда её выгодно сжать, и без сжатия она вылезет за поле.
        // Последняя строка абзаца не выключается — её короткость это конец
        // мысли, а не изъян набора, — но если и она переполнена, сжать надо
        // и её: за поле не должно вылезать ничего.
        const bool overfull = natural > available + 0.05f;
        const bool wantsJustification = style.alignment == Alignment::Justify && !lastLine;

        if (natural > 0.0f && (wantsJustification || overfull))
            impl.justify(runs, available);

        float x = index == 0 ? indent : 0.0f;
        float total = 0.0f;
        for (const LineRun& run : runs)
            total += run.glyphs.width;

        switch (style.alignment) {
        case Alignment::Right:  x += available - total; break;
        case Alignment::Center: x += (available - total) * 0.5f; break;
        default: break;
        }

        for (LineRun& run : runs) {
            run.glyphs.originX = x;
            x += run.glyphs.width;
            line.runs.push_back(std::move(run.glyphs));
        }
        line.width = total;

        // Знаки сносок, попавшие на эту строку. Надстрочное начертание всегда
        // отрезает знак в отдельные прогоны, поэтому его место на строке — это
        // просто объединение прогонов, лежащих внутри него; отдельно измерять
        // ничего не нужно.
        for (const NoteAnchor& note : paragraph.notes) {
            if (note.position + note.length <= from || note.position >= to)
                continue;

            PlacedNote placed;
            placed.target = note.target;
            bool started = false;

            for (const GlyphRun& run : line.runs) {
                if (run.textStart + run.textLength <= note.position ||
                    run.textStart >= note.position + note.length)
                    continue;

                if (!started) {
                    placed.x = run.originX;
                    started = true;
                }
                placed.width += run.width;
            }

            if (started)
                line.notes.push_back(placed);
        }

        if (line.ascent == 0.0f) {
            // Строка без единого глифа всё равно занимает высоту: иначе пустая
            // строка между строфами исчезнет.
            IDWriteFontFace* const face = impl.baseFace(impl.style.fontFamily.c_str(), style.bold, style.italic);
            const DWRITE_FONT_METRICS& fontMetrics = impl.metricsOf(face);
            const float emScale = style.fontSize / static_cast<float>(fontMetrics.designUnitsPerEm);
            line.ascent = fontMetrics.ascent * emScale;
            line.descent = fontMetrics.descent * emScale;
        }

        line.height = std::max(impl.lineHeightOf(style), (line.ascent + line.descent) * 1.05f);

        lines.push_back(std::move(line));
        from = at.textEnd;
    }

    return lines;
}

}  // namespace bukvitsa::typography
