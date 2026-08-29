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

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
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

    /// Книга, которую продолжит большая кнопка: слева обложка, под своей
    /// надписью — название, под названием — автор. Пустое название оставляет
    /// кнопку простой надписью: продолжать пока нечего.
    void setContinueBook(std::wstring_view title, std::wstring_view author,
                         const std::filesystem::path& cover);

    /// Что делают кнопки. Пустой обработчик значит «кнопка на месте, но
    /// делать ей пока нечего» — так и задумано для каталога.
    ///
    /// «Продолжить чтение» — действие по умолчанию, его зовёт Enter;
    /// «Выйти из читалки» — отмена, её зовёт Escape.
    std::function<void()> onContinueReading;
    std::function<void()> onLibrary;
    std::function<void()> onAddBook;
    std::function<void()> onAddFolder;
    std::function<void()> onExit;

private:
    /// Одна ширина на всех; главную от прочих отличают высота и кегль, а
    /// кнопку отмены — чуть более серое лицо.
    wxl::Button addButton(std::wstring_view caption, float tall, float kegel,
                          std::function<void()>* action, bool cancel = false);

    wxl::Compositor compositor_;
    wxl::Nullable<wxl::Grid> root_ = nullptr;
    std::vector<wxl::Visual> revealing_;   ///< визуалы кнопок в порядке появления

    /// Визуал обёртки карточки. Проступает вместе с кнопками, но своего
    /// визуала у карточки не отнять: у неё заняты фасадные свойства (подъём
    /// по Z несёт тень), а мешать их с handout-визуалом нельзя — ломается
    /// попадание мыши. Подробности — у построения карточки в .cpp.
    wxl::Nullable<wxl::Visual> cardVisual_ = nullptr;

    /// Большая кнопка и то, чем она наполнена: setContinueBook() зовут на
    /// каждом показе экрана, и одинаковое наполнение не перестраивается.
    wxl::Nullable<wxl::Button> continueButton_ = nullptr;
    std::wstring continueKey_;

    bool revealed_ = false;
};

}  // namespace bukvitsa::reader
