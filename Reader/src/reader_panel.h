#pragma once
// Панель читалки: оглавление, поиск, закладки и настройки — в одном ящике,
// который выезжает слева поверх страницы.
//
// Одна панель на четыре дела, а не четыре всплывашки, потому что дело у них
// одно: читатель на секунду отвлёкся от текста, чтобы куда-то перейти или
// что-то подкрутить, и хочет вернуться. Один Escape закрывает всё, одно место
// на экране, один вид.
//
// Ящик выезжает поверх страницы, а не отодвигает её: полоса набора при смене
// ширины перевёрстывается целиком, и открывать оглавление было бы дороже, чем
// перелистнуть. Поверх — бесплатно, и место чтения не дёргается.
//
// Панель — часть обстановки, а не бумаги, поэтому она тёмная при любой теме
// страницы: светлый ящик поверх ночной страницы слепит, а тёмный поверх
// дневной читается как ящик, чем он и является.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core.
#include <functional>
#include <vector>

#include "library.h"

#include "Object.h"
#include "pch.h"

// Последним: он ведёт к модели книги, а она импортирует wxl.text, после чего
// стандартный заголовок MSVC уже не принимает.
#include "book_view.h"

namespace bukvitsa::reader {

class ReaderPanel {
public:
    enum class Tab { Contents, Search, Bookmarks, Settings };

    /// @param view полоса набора: у неё панель и спрашивает книгу, и ей же
    ///        отдаёт переходы. Панель без книги бессмысленна, поэтому связь
    ///        прямая, а не через десяток обработчиков.
    ReaderPanel(const wxl::Compositor& compositor, BookView& view);

    /// Элемент, который кладут поверх полосы набора.
    const wxl::UIElement& root() const { return root_.value(); }

    /// Открывает панель на этой вкладке. Открытая на другой — переключается.
    void open(Tab tab);
    void close();
    bool isOpen() const { return open_; }

    /// Закладки текущей книги. Панель их показывает и меняет, а хранит и
    /// пишет приложение: файл книги — не её забота.
    void setState(BookState* state);

    /// Закладки изменились — пора записать состояние книги.
    std::function<void()> onStateChanged;

    /// Настройка изменилась — пора записать settings.xml.
    std::function<void()> onSettingsChanged;

    /// Читатель попросился на полку — выбрать другую книгу. Панель этого не
    /// умеет и не должна: смена книги — дело приложения.
    std::function<void()> onLibrary;

    /// Читатель попросил новую обложку. Мастер — оверлей приложения, а не
    /// панели: он ложится поверх всей полосы.
    std::function<void()> onAddSkin;

    /// Шестерёнка у обложки: открыть в мастере правку её кривизны.
    std::function<void(std::wstring)> onEditSkin;

    /// Пересобирает список тем: встроенные плюс обложки из полосы набора.
    /// Зовётся приложением, когда реестр обложек изменился.
    void refreshThemes();

private:
    void buildTree();
    wxl::UIElement buildContents();
    wxl::UIElement buildSearch();
    wxl::UIElement buildBookmarks();
    wxl::UIElement buildSettings();

    /// Кнопка вкладки: заголовок плюс переключение.
    wxl::Button tabButton(std::wstring_view caption, Tab tab);

    /// Строка списка — то, из чего собраны все три списка панели.
    wxl::Button listItem(std::wstring_view caption, std::wstring_view under, float indent,
                         std::function<void()> action);

    void showTab(Tab tab);

    /// Отмечает кнопку нынешней темы — как отмечена открытая вкладка.
    void markTheme();

    void fillContents();
    void fillBookmarks();
    void runSearch();
    void toggleBookmark();
    void syncSettings();

    wxl::Compositor compositor_;
    BookView& view_;
    BookState* state_ = nullptr;

    wxl::core::nullable<wxl::Border> root_ = nullptr;
    wxl::core::nullable<wxl::Visual> visual_ = nullptr;   ///< для выезда и ухода

    wxl::core::nullable<wxl::Grid> pages_ = nullptr;
    std::vector<wxl::UIElement> tabPages_;
    std::vector<wxl::Button> tabButtons_;
    std::vector<wxl::Button> themeButtons_;
    wxl::core::nullable<wxl::StackPanel> themesPanel_ = nullptr;   ///< пересобирается

    wxl::core::nullable<wxl::StackPanel> contentsList_ = nullptr;
    wxl::core::nullable<wxl::StackPanel> searchList_ = nullptr;
    wxl::core::nullable<wxl::StackPanel> bookmarkList_ = nullptr;
    wxl::core::nullable<wxl::TextBox> searchBox_ = nullptr;
    wxl::core::nullable<wxl::TextBlock> searchNote_ = nullptr;
    wxl::core::nullable<wxl::TextBlock> bookmarkNote_ = nullptr;

    wxl::core::nullable<wxl::Slider> fontSize_ = nullptr;
    wxl::core::nullable<wxl::Slider> lineHeight_ = nullptr;
    wxl::core::nullable<wxl::Slider> margin_ = nullptr;

    Tab tab_ = Tab::Contents;
    bool open_ = false;
    bool filling_ = false;   ///< правим ползунки сами — не считать это за ввод
};

}  // namespace bukvitsa::reader
