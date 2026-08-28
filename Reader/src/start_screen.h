#pragma once
// Стартовый экран: заставка, а поверх неё справа — кнопки.
//
// Отдельным окном заставку не делаем намеренно: второе окно завело бы вторую
// кнопку на панели задач, которая появляется и исчезает. Это просто
// содержимое главного окна, которое потом сменится полосой набора.
//
// Класс не наследуется от визуальных компонент, а держит их внутри себя,
// включая корневой UIElement: описание остаётся декларативным, а поведение —
// обычным событийным кодом на честных событиях WinUI.

#include <functional>
#include <vector>

#include "Nullable.h"
#include "pch.h"

namespace bukvitsa::reader {

// TODO: : public wxl::core:noncopyable
class StartScreen {
public:
    /// Строит дерево целиком. Кнопки создаются уже прозрачными и приподнятыми:
    /// поставь прозрачность в Loaded — и они успеют показаться на полную ровно
    /// один кадр, чего хватает, чтобы мигнуть.
    explicit StartScreen(const wxl::Compositor& compositor);

    /// Корень, который отдаётся окну как содержимое.
    const wxl::UIElement& root() const { return root_.value(); }

    /// Проявление с разбегом. Зовётся, когда приложение готово: до этого
    /// момента на экране одна заставка.
    void reveal();

    /// Что делают кнопки. Пустой обработчик значит «кнопка на месте, но
    /// делать ей пока нечего» — так и задумано для каталога.
    std::function<void()> onContinueReading;
    std::function<void()> onLibrary;
    std::function<void()> onAddBook;
    std::function<void()> onAddFolder;

private:
    /// Одна ширина на всех; главную от прочих отличают высота и кегль.
    wxl::Button addButton(std::wstring_view caption, float tall, float kegel,
                          std::function<void()>* action);

    wxl::Compositor compositor_;
    wxl::Nullable<wxl::Grid> root_ = nullptr;
    std::vector<wxl::Visual> revealing_;   ///< визуалы кнопок в порядке появления
    bool revealed_ = false;
};

}  // namespace bukvitsa::reader
