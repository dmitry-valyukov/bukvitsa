#pragma once
// Витрина хранилища: полка с книгами, которые читалка знает.
//
// Полка строится обычным циклом, а не разметкой с шаблоном элемента: книг
// столько, сколько их в реестре, и это число известно только во время работы,
// тогда как повторители нашего синтаксиса (`repeat`, `iterate`) считают на
// этапе компиляции. Цикл, добавляющий детей в панель, — то же самое, только
// без посредника, и читается он лучше, чем шаблон с привязкой.
//
// Как и остальные экраны, класс не наследуется от визуальных компонент, а
// держит их внутри себя.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core.
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "library.h"

#include "Object.h"
#include "pch.h"

namespace bukvitsa::reader {

class LibraryScreen {
public:
    LibraryScreen();

    /// Корень, который отдаётся окну как содержимое.
    const wxl::UIElement& root() const { return root_.value(); }

    /// Перестраивает полку под содержимое реестра. Зовётся каждый раз, когда
    /// витрину показывают: книга могла добавиться, а место чтения — уехать.
    void show(const Library& library, bool continueAtStart);

    /// Ставит на полку ещё одну книгу -- ту, которую только что разобрал обход
    /// каталога. Полка при этом не пересобирается: карточки, которые уже стоят,
    /// остаются как есть вместе со своим прогрессом.
    void appendBook(const BookEntry& entry);

    /// Дописывает на карточке строку прогресса -- ту самую, которой при показе
    /// ещё не было, потому что файл состояния книги читается в фоне.
    ///
    /// Ничего не делает, если полку с тех пор пересобрали: карточки той книги
    /// уже нет, а есть новая, и её прогресс придёт своим чередом.
    void setProgress(std::wstring_view guid, std::uint32_t charOffset, std::size_t bookmarks);

    std::function<void(std::wstring)> onOpen;   ///< guid выбранной книги
    std::function<void()> onAddBook;
    std::function<void()> onBack;
    std::function<void(bool)> onContinueAtStartChanged;

private:
    wxl::Button shelfItem(const BookEntry& entry);

    wxl::core::nullable<wxl::Grid> root_ = nullptr;
    wxl::core::nullable<wxl::StackPanel> shelf_ = nullptr;
    wxl::core::nullable<wxl::CheckBox> continueBox_ = nullptr;
    wxl::core::nullable<wxl::TextBlock> emptyNote_ = nullptr;

    /// Строка прогресса каждой карточки, по guid книги. Живёт ровно от одного
    /// показа полки до другого -- как и сами карточки.
    std::map<std::wstring, wxl::TextBlock> progress_;

    /// Реестр, по которому построена нынешняя полка. Не владеет: реестр живёт
    /// в приложении и переживает витрину.
    const Library* shown_ = nullptr;
};

}  // namespace bukvitsa::reader
