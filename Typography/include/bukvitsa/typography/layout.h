#pragma once
// Вёрстка абзаца в строки глифов.
//
// Разделение труда с DirectWrite ровно такое, как записано в docs/decisions.md:
// система делает итемизацию, BiDi, шейпинг, подбор запасного шрифта, ищет места
// возможных переломов и распределяет выключку по глифам; своими остаются
// разбивка абзаца на строки (Кнут-Пласс, linebreak.h) и всё книжное поверх
// строк.
//
// Что отдаётся наружу — прогоны глифов, готовые к ID2D1DeviceContext::
// DrawGlyphRun. Никакого промежуточного представления «строка текста» нет
// намеренно: между вёрсткой и растеризацией не должно быть места, где текст
// можно измерить иначе, чем его нарисуют.
//
// Владение: шрифты живут в Engine, а прогоны на них только ссылаются. Значит,
// строки нельзя пережить движком — что естественно, ведь при смене кегля
// перевёрстывается всё.

// Через block.h, а не своими: там собрано объединение стандартных заголовков
// всех публичных заголовков вёрстки и стоит импорт, после которого стандартный
// заголовок MSVC уже не примет.
#include "bukvitsa/typography/block.h"

namespace bukvitsa::typography {

/// Как строка выключается по полосе.
enum class Alignment : std::uint8_t { Left, Right, Center, Justify };

/// Шрифт и язык — то, что задаётся один раз на книгу.
struct TextStyle {
    std::wstring fontFamily = L"Georgia";
    std::wstring monospaceFamily = L"Consolas";
    std::wstring locale = L"ru-RU";
    float fontSize = 20.0f;        ///< кегль основного текста, DIP
    float lineHeight = 1.45f;      ///< множитель кегля
};

/// Как набирается один блок. Всё, чем заголовок отличается от абзаца.
struct ParagraphStyle {
    float fontSize = 20.0f;
    float lineHeight = 1.45f;
    float firstLineIndent = 0.0f;  ///< DIP; отрицательный — висячая строка
    Alignment alignment = Alignment::Justify;
    bool bold = false;
    bool italic = false;
    bool monospace = false;
};

/// Прогон глифов одного шрифта — аргумент DrawGlyphRun в готовом виде.
struct GlyphRun {
    IDWriteFontFace* fontFace = nullptr;   ///< живёт в Engine
    float fontSize = 0.0f;
    std::uint8_t bidiLevel = 0;
    FontStyle style;                       ///< подчёркивание и зачёркивание рисует читалка

    std::vector<std::uint16_t> glyphIndices;
    std::vector<float> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;

    /// Кусок Paragraph::text, из которого набран прогон. Нужен всем, кто
    /// связывает нарисованное с исходным текстом: знаку сноски, подсветке
    /// найденного, выделению.
    std::uint32_t textStart = 0;
    std::uint32_t textLength = 0;

    float originX = 0.0f;                  ///< от левого края полосы
    float width = 0.0f;
};

/// Знак сноски, попавший на эту строку, — уже в координатах строки, чтобы
/// поймать по нему щелчок, ничего больше не пересчитывая.
struct PlacedNote {
    const fb3::Node* target = nullptr;
    float x = 0.0f;                        ///< от левого края полосы
    float width = 0.0f;
};

/// Одна набранная строка.
struct Line {
    std::vector<GlyphRun> runs;
    std::vector<PlacedNote> notes;

    float ascent = 0.0f;                   ///< от базовой линии вверх
    float descent = 0.0f;
    float height = 0.0f;                   ///< с учётом межстрочного множителя
    float width = 0.0f;                    ///< фактически занятое место

    std::uint32_t textStart = 0;           ///< в Paragraph::text
    std::uint32_t textLength = 0;
    std::uint32_t charOffset = 0;          ///< позиция первого символа строки в книге
};

/// Абзац, прошедший анализ и шейпинг, — то, что не надо делать заново.
///
/// Ни итемизация, ни BiDi, ни поиск переломов, ни подбор глифов не зависят ни
/// от ширины полосы, ни от кегля: `GetGlyphs` не принимает размера шрифта
/// вовсе. От кегля зависят только метрики — ширины, сдвиги, возможности
/// выключки, — и зависят линейно, потому что естественный режим измерения
/// DirectWrite считает их как «единицы дизайна × кегль ÷ единиц на кегель».
///
/// Отсюда весь смысл: шейпинг делается один раз на книгу, а смена кегля и
/// размера окна стоят одной разбивки на строки — на романе это разница между
/// четвертью секунды и сорока миллисекундами.
///
/// Владение: внутри лежат виды в текст абзаца и указатели на шрифты движка,
/// поэтому объект нельзя пережить ни абзацу, ни `Engine`.
class ShapedParagraph {
public:
    ~ShapedParagraph();

    ShapedParagraph(const ShapedParagraph&) = delete;
    ShapedParagraph& operator=(const ShapedParagraph&) = delete;

    /// Кегль, на котором абзац отшейпили. Вёрстка на другом кегле пересчитывает
    /// метрики отношением, не обращаясь к DirectWrite заново.
    float referenceFontSize() const;

private:
    friend class Engine;
    ShapedParagraph();

    struct Data;
    std::unique_ptr<Data> data_;
};

using ShapedParagraphPtr = std::shared_ptr<ShapedParagraph>;

/// Движок вёрстки: держит DirectWrite и кэш шрифтов, верстает абзацы.
///
/// Один на приложение (шрифты кэшируются), не потокобезопасен: DirectWrite
/// позволяет, но у читалки вёрстка идёт на своём потоке целиком.
class Engine {
public:
    /// @param factory фабрика DirectWrite; читалка берёт её у CompositionHost,
    ///        тесты создают свою.
    explicit Engine(IDWriteFactory* factory);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void setTextStyle(const TextStyle& style);
    const TextStyle& textStyle() const;

    /// Анализирует и шейпит абзац — самая дорогая часть вёрстки, та, которую
    /// стоит сохранить. Кегль из `style` запоминается как опорный.
    ShapedParagraphPtr shape(const Paragraph& paragraph, const ParagraphStyle& style);

    /// Разбивает отшейпленный абзац на строки шириной width. Дёшево: обращений
    /// к DirectWrite здесь только выключка готовых строк.
    ///
    /// Если стиль разошёлся с тем, на котором абзац шейпили, не размером
    /// (сменилось начертание или шрифт книги), абзац шейпится заново на месте —
    /// результат верен всегда, сэкономлено только когда звали правильно.
    std::vector<Line> layout(const ShapedParagraph& shaped, float width,
                             const ParagraphStyle& style);

    /// То же, но с середины абзаца: первая строка начинается ровно с символа
    /// firstChar, а не с начала абзаца.
    ///
    /// Отдельный вызов, а не нулевой аргумент у `layout`, потому что это
    /// другой случай: абзац, набранный с начала, получает красную строку и
    /// целиком укладывается в строки; абзац, набранный с середины, — хвост
    /// страницы, начатой с места чтения. Этим живёт мгновенная вёрстка
    /// текущей страницы: полоса сменилась, а первая буква на ней осталась той
    /// же, и читатель видит новый кегль не дожидаясь, пока пересчитается вся
    /// книга.
    std::vector<Line> layoutFrom(const ShapedParagraph& shaped, std::uint32_t firstChar,
                                 float width, const ParagraphStyle& style);

    /// Верстает абзац в строки шириной width — шейпинг и разбивка разом.
    ///
    /// Пустой абзац даёт пустой список: блок без текста не занимает полосы.
    std::vector<Line> layout(const Paragraph& paragraph, float width, const ParagraphStyle& style);

    /// Высота строки при таком кегле — нужна пагинатору до вёрстки, чтобы
    /// прикинуть, влезет ли блок.
    float lineHeightFor(const ParagraphStyle& style) const;

private:
    /// Общее тело обоих `layout`: разбивка от символа `firstChar` до конца
    /// абзаца.
    std::vector<Line> layoutRange(const ShapedParagraph& shaped, std::uint32_t firstChar,
                                  float width, const ParagraphStyle& style);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bukvitsa::typography
