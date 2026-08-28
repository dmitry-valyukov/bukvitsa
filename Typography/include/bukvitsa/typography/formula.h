#pragma once
// Формулы: MicroTeX разбирает LaTeX в дерево боксов, а рисует его Typography —
// реализацией tex::Graphics2D поверх Direct2D и DirectWrite (formula.cpp).
// Бэкенд, а не SVG-обход, по решению: страница и так рисуется прямыми
// командами в ID2D1DeviceContext, и формула рисуется теми же — тем же
// сглаживанием, тем же цветом темы, без посредников. Сам MicroTeX лежит в
// external/MicroTeX, см. README там же.
//
// Потоки: MicroTeX держит контекст и кэши шрифтов в статиках, поэтому и
// движок, и разбор, и отрисовка готовых формул зовутся с одного потока — или
// под внешней синхронизацией. У читалки это поток вёрстки; как формула
// попадает в кадр — решится вместе со встраиванием в Reader.

// Через block.h, а не своими: там собрано объединение стандартных заголовков
// всех публичных заголовков вёрстки и стоит импорт, после которого стандартный
// заголовок MSVC уже не примет.
#include "bukvitsa/typography/block.h"

namespace bukvitsa::typography {

/// Готовая к показу формула.
///
/// Владение: внутри — дерево боксов MicroTeX; жить формуле можно и дольше
/// движка, но рисовать её после его смерти нельзя — шрифты живут в движке.
class Formula {
public:
    ~Formula();

    Formula(const Formula&) = delete;
    Formula& operator=(const Formula&) = delete;

    /// Ширина бокса формулы, DIP.
    float width() const;

    /// Полная высота бокса, DIP: подъём плюс свес.
    float height() const;

    /// Базовая линия, DIP от верха бокса, — то, чем формула выравнивается с
    /// текстом строки, когда стоит в ней инлайн-боксом.
    float baseline() const;

    /// Рисует формулу; x, y — левый верхний угол в координатах контекста.
    /// Цвет задан при разборе; трансформация контекста уважается и
    /// восстанавливается.
    void draw(ID2D1DeviceContext* context, float x, float y) const;

private:
    friend class FormulaEngine;

    struct Impl;
    explicit Formula(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

/// Движок формул: инициализация MicroTeX и разбор LaTeX.
///
/// Один на приложение — MicroTeX хранит контекст в статиках, и второй движок
/// не завёлся бы честно. Шрифты формул (OTF из res) создаются через переданную
/// фабрику DirectWrite и кэшируются внутри MicroTeX.
class FormulaEngine {
public:
    /// @param factory фабрика DirectWrite; переживает движок.
    /// @param resourceRoot каталог res из поставки MicroTeX
    ///        (external/MicroTeX/res). Путь ASCII: внутренности MicroTeX
    ///        читают его через fopen, и на других обещаний нет.
    /// @param serifFamily семейство для `\\text{...}` и текста вне
    ///        математических алфавитов — сюда отдаётся шрифт книги.
    FormulaEngine(IDWriteFactory* factory, const std::string& resourceRoot,
                  std::wstring serifFamily = L"Georgia",
                  std::wstring sansFamily = L"Segoe UI");
    ~FormulaEngine();

    FormulaEngine(const FormulaEngine&) = delete;
    FormulaEngine& operator=(const FormulaEngine&) = delete;

    /// Разбирает формулу в готовый бокс.
    ///
    /// @param tex формула без обрамляющих $...$
    /// @param textSize кегль окружающего текста, DIP
    /// @param maxWidth ширина полосы — окружениям с выравниванием; сама
    ///        формула на строки не переносится
    /// @param argb цвет, как у темы страницы; смена цвета — новый разбор
    ///        (он дёшев, кэш — дело вызывающего)
    /// @return формула, или nullptr на вход, которого MicroTeX не простил.
    ///         Прощает он многое — оборванные скобки, незнакомые команды —
    ///         и тогда возвращает формулу с тем, что понял; исключений на
    ///         чужой вход не бывает в любом случае.
    std::unique_ptr<Formula> parse(std::wstring_view tex, float textSize, float maxWidth,
                                   std::uint32_t argb = 0xFF000000);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bukvitsa::typography
