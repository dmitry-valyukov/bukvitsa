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
#include <functional>
#include <string>

#include "library.h"

#include "Nullable.h"
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

    std::function<void(std::wstring)> onOpen;   ///< guid выбранной книги
    std::function<void()> onAddBook;
    std::function<void()> onBack;
    std::function<void(bool)> onContinueAtStartChanged;

private:
    wxl::Button shelfItem(const BookEntry& entry);

    wxl::Nullable<wxl::Grid> root_ = nullptr;
    wxl::Nullable<wxl::StackPanel> shelf_ = nullptr;
    wxl::Nullable<wxl::CheckBox> continueBox_ = nullptr;
    wxl::Nullable<wxl::TextBlock> emptyNote_ = nullptr;
};

}  // namespace bukvitsa::reader
