#include <algorithm>
#include <format>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "reader_panel.h"

#include "book_index.h"

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

namespace {

// Ящик, а не бумага: свои цвета при любой теме страницы.
constexpr uint32_t kChrome = 0xF21E1E22;   ///< слегка прозрачный — под ним текст
constexpr uint32_t kInk = 0xFFE8E4DC;
constexpr uint32_t kDim = 0xFF9A968E;
constexpr uint32_t kEdge = 0x33FFFFFF;
constexpr uint32_t kActive = 0x22FFFFFF;

constexpr double kWidth = 380;

// Выезд: короткий, потому что панель открывают между двумя строчками текста и
// ждать её не должны. Двести миллисекунд — предел, за которым появление
// читается как задержка.
constexpr auto kSlide = 180ms;

/// Строчными — для сравнения без учёта регистра там, где нужен только ответ
/// «пусто или нет».
bool blank(std::wstring_view text) {
    return std::all_of(text.begin(), text.end(), [](wchar_t c) { return c == L' ' || c == L'\t'; });
}

}  // namespace

ReaderPanel::ReaderPanel(const Compositor& compositor, BookView& view)
    : compositor_(compositor), view_(view) {
    buildTree();
}

void ReaderPanel::buildTree() {
    using namespace wxl::dsl;

    tabPages_ = {buildContents(), buildSearch(), buildBookmarks(), buildSettings()};

    // Выход на полку — первым, отдельной строкой над вкладками. Вкладки
    // говорят о книге, которая открыта; эта кнопка — о том, чтобы открыть
    // другую, и стоять в одном ряду с ними ей не за что.
    auto shelf = Button{
        row = 0,
        L"←  Моя библиотека",
        hAlign.stretch,
        horizontalContentAlignment = HorizontalAlignment::Left,
        fontSize = 14,
        Margin{12, 12, 12, 0},
        Padding{12, 8},
        foreground = SolidColorBrush{ARGB{kInk}},
        background = SolidColorBrush{ARGB{0x00000000}},
        borderBrush = SolidColorBrush{ARGB{kEdge}},
        BorderThickness{1},
        CornerRadius{4},
        onClick =
            [this](Object const&, RoutedEventArgs&) {
                if (onLibrary) onLibrary();
            },
    };

    auto strip = StackPanel{
        row = 1,
        Orientation::Horizontal,
        Margin{12, 12, 12, 6},
    };
    for (auto&& [caption, tab] : {std::pair{L"Оглавление", Tab::Contents},
                                  std::pair{L"Поиск", Tab::Search},
                                  std::pair{L"Закладки", Tab::Bookmarks},
                                  std::pair{L"Вид", Tab::Settings}}) {
        auto button = tabButton(caption, tab);
        tabButtons_.push_back(button);
        strip.children().append(button);
    }

    pages_ = Grid{row = 2, Margin{12, 6, 12, 12}};
    for (const UIElement& page : tabPages_) {
        pages_.value().children().append(page);
    }

    root_ = Border{
        hAlign.left,
        vAlign.stretch,
        width = kWidth,
        visibility = Visibility::Collapsed,
        background = SolidColorBrush{ARGB{kChrome}},
        borderBrush = SolidColorBrush{ARGB{kEdge}},
        BorderThickness{0, 0, 1, 0},
        Grid{
            rowDefinitions = L"auto,auto,*",
            shelf,
            strip,
            pages_.value(),
        },
    };

    // Выезд идёт по Translation, а не по Offset: Offset — это то, чем XAML
    // расставляет элементы при разметке, и анимация его отобрала бы.
    ElementCompositionPreview::setIsTranslationEnabled(root_.value(), true);
    visual_ = ElementCompositionPreview::getElementVisual(root_.value());
    visual_.value().properties().insertVector3(L"Translation",
                                               Vector3{-static_cast<float>(kWidth), 0.0f, 0.0f});

    showTab(Tab::Contents);
}

Button ReaderPanel::tabButton(std::wstring_view caption, Tab tab) {
    using namespace wxl::dsl;

    return Button{
        caption,
        fontSize = 14,
        Margin{0, 0, 6, 0},
        Padding{12, 6},
        foreground = SolidColorBrush{ARGB{kInk}},
        background = SolidColorBrush{ARGB{0x00000000}},
        borderBrush = SolidColorBrush{ARGB{kEdge}},
        BorderThickness{1},
        CornerRadius{4},
        onClick = [this, tab](Object const&, RoutedEventArgs&) { open(tab); },
    };
}

Button ReaderPanel::listItem(std::wstring_view caption, std::wstring_view under, float indent,
                             std::function<void()> action) {
    using namespace wxl::dsl;

    auto lines = StackPanel{
        TextBlock{
            caption,
            fontSize = 14,
            foreground = SolidColorBrush{ARGB{kInk}},
            textWrapping.wrap,
            maxLines = 2,
            textTrimming.characterEllipsis,
        },
    };

    if (!under.empty()) {
        lines.children().append(TextBlock{
            under,
            fontSize = 12,
            foreground = SolidColorBrush{ARGB{kDim}},
            Margin{0, 2, 0, 0},
            maxLines = 1,
            textTrimming.characterEllipsis,
        });
    }

    return Button{
        hAlign.stretch,
        horizontalContentAlignment = HorizontalAlignment::Stretch,
        Margin{indent, 1, 0, 1},
        Padding{8, 6},
        background = SolidColorBrush{ARGB{0x00000000}},
        borderBrush = SolidColorBrush{ARGB{0x00000000}},
        BorderThickness{0},
        CornerRadius{4},
        onClick = [action](Object const&, RoutedEventArgs&) { if (action) action(); },
        content = lines,
    };
}

/* ---------------- вкладки ---------------- */

UIElement ReaderPanel::buildContents() {
    using namespace wxl::dsl;

    contentsList_ = StackPanel{};
    return ScrollViewer{
        horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
        content = contentsList_.value(),
    };
}

UIElement ReaderPanel::buildSearch() {
    using namespace wxl::dsl;

    searchBox_ = TextBox{
        row = 0,
        placeholderText = L"Что искать",
        Margin{0, 0, 0, 8},
    };

    // Поиск по Enter, а не по каждой букве: искать по одной букве в романе —
    // это тысячи находок, из которых читателю не нужна ни одна.
    searchBox_.value().add_onKeyDown([this](Object const&, KeyRoutedEventArgs& args) {
        if (args.key() != VirtualKey::Enter) return;
        runSearch();
        args.handled(true);
    });

    searchNote_ = TextBlock{
        row = 1,
        L"Введите слово и нажмите Enter.",
        fontSize = 13,
        foreground = SolidColorBrush{ARGB{kDim}},
        textWrapping.wrap,
    };

    searchList_ = StackPanel{};

    return Grid{
        rowDefinitions = L"auto,auto,*",
        searchBox_.value(),
        searchNote_.value(),
        ScrollViewer{
            row = 2,
            horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
            content = searchList_.value(),
        },
    };
}

UIElement ReaderPanel::buildBookmarks() {
    using namespace wxl::dsl;

    bookmarkNote_ = TextBlock{
        row = 1,
        fontSize = 13,
        foreground = SolidColorBrush{ARGB{kDim}},
        textWrapping.wrap,
    };

    bookmarkList_ = StackPanel{};

    return Grid{
        rowDefinitions = L"auto,auto,*",
        Button{
            row = 0,
            L"Заложить эту страницу",
            hAlign.stretch,
            Margin{0, 0, 0, 8},
            foreground = SolidColorBrush{ARGB{kInk}},
            background = SolidColorBrush{ARGB{kActive}},
            borderBrush = SolidColorBrush{ARGB{kEdge}},
            BorderThickness{1},
            CornerRadius{4},
            onClick = [this](Object const&, RoutedEventArgs&) { toggleBookmark(); },
        },
        bookmarkNote_.value(),
        ScrollViewer{
            row = 2,
            horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
            content = bookmarkList_.value(),
        },
    };
}

UIElement ReaderPanel::buildSettings() {
    using namespace wxl::dsl;

    auto caption = [](std::wstring_view said) {
        return TextBlock{
            said,
            fontSize = 13,
            foreground = SolidColorBrush{ARGB{kDim}},
            Margin{0, 12, 0, 2},
        };
    };

    themesPanel_ = StackPanel{};
    refreshThemes();

    // Ползунок, а не пара кнопок: кегль подбирают, а не выставляют числом, и
    // видеть весь ход сразу удобнее, чем нажимать «плюс» восемь раз.
    auto slider = [](double from, double to, double step) {
        return Slider{
            minimum = from,
            maximum = to,
            stepFrequency = step,
            Margin{0, 0, 0, 4},
        };
    };

    fontSize_ = slider(10, 48, 1);
    lineHeight_ = slider(100, 240, 5);
    margin_ = slider(2, 25, 0.5);

    fontSize_.value().add_onValueChanged([this](Object const&, RangeBaseValueChangedEventArgs& args) {
        if (filling_) return;
        view_.setFontSize(static_cast<float>(args.newValue()));
        if (onSettingsChanged) onSettingsChanged();
    });
    lineHeight_.value().add_onValueChanged([this](Object const&, RangeBaseValueChangedEventArgs& args) {
        if (filling_) return;
        view_.setLineHeight(static_cast<float>(args.newValue()) / 100.0f);
        if (onSettingsChanged) onSettingsChanged();
    });
    margin_.value().add_onValueChanged([this](Object const&, RangeBaseValueChangedEventArgs& args) {
        if (filling_) return;
        view_.setMargin(static_cast<float>(args.newValue()) / 100.0f);
        if (onSettingsChanged) onSettingsChanged();
    });

    return ScrollViewer{
        horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
        content = StackPanel{
            caption(L"Тема"),
            themesPanel_.value(),
            caption(L"Кегль"),
            fontSize_.value(),
            caption(L"Интерлиньяж"),
            lineHeight_.value(),
            caption(L"Поля"),
            margin_.value(),
            TextBlock{
                L"Кегль меняется ещё и Ctrl с колесом, а тема — клавишей T.",
                fontSize = 12,
                foreground = SolidColorBrush{ARGB{kDim}},
                Margin{0, 16, 0, 0},
                textWrapping.wrap,
            },
        },
    };
}

/* ---------------- показ ---------------- */

void ReaderPanel::showTab(Tab tab) {
    tab_ = tab;
    for (std::size_t i = 0; i < tabPages_.size(); ++i) {
        tabPages_[i].visibility(static_cast<std::size_t>(tab) == i ? Visibility::Visible
                                                                  : Visibility::Collapsed);
    }
    for (std::size_t i = 0; i < tabButtons_.size(); ++i) {
        tabButtons_[i].background(
            SolidColorBrush{ARGB{static_cast<std::size_t>(tab) == i ? kActive : 0x00000000u}});
    }
}

void ReaderPanel::open(Tab tab) {
    showTab(tab);

    switch (tab) {
        case Tab::Contents: fillContents(); break;
        case Tab::Bookmarks: fillBookmarks(); break;
        case Tab::Settings: syncSettings(); break;
        case Tab::Search: break;   // список остаётся от прошлого поиска
    }

    if (!open_) {
        open_ = true;
        root_.value().visibility(Visibility::Visible);

        auto slide = compositor_.createVector3KeyFrameAnimation();
        slide.duration(kSlide);
        slide.insertKeyFrame(1.0f, Vector3{0.0f, 0.0f, 0.0f},
                             compositor_.createLinearEasingFunction());
        visual_.value().startAnimation(L"Translation", slide);
    }

    if (tab == Tab::Search) searchBox_.value().focus(FocusState::Programmatic);
}

void ReaderPanel::close() {
    if (!open_) return;
    open_ = false;

    // Уехавшую панель надо ещё и спрятать, иначе она продолжит ловить щелчки
    // за краем экрана. Конец анимации узнаётся пакетом, а не таймером: пакет
    // сам скажет, когда последняя анимация в нём закончилась.
    auto batch = compositor_.createScopedBatch(CompositionBatchTypes::Animation);

    auto slide = compositor_.createVector3KeyFrameAnimation();
    slide.duration(kSlide);
    slide.insertKeyFrame(1.0f, Vector3{-static_cast<float>(kWidth), 0.0f, 0.0f},
                         compositor_.createLinearEasingFunction());
    visual_.value().startAnimation(L"Translation", slide);

    batch.add_onCompleted([this](Object const&, CompositionBatchCompletedEventArgs&) {
        if (!open_) root_.value().visibility(Visibility::Collapsed);
    });
    batch.end();
}

void ReaderPanel::setState(BookState* state) {
    state_ = state;
    if (tab_ == Tab::Bookmarks) fillBookmarks();
}

/* ---------------- содержимое вкладок ---------------- */

void ReaderPanel::fillContents() {
    contentsList_.value().children().clear();

    const std::vector<ContentsEntry> contents = contentsOf(view_.blocks());
    if (contents.empty()) {
        contentsList_.value().children().append(listItem(L"В этой книге нет заголовков", {}, 0,
                                                         {}));
        return;
    }

    for (const ContentsEntry& entry : contents) {
        const std::uint32_t offset = entry.charOffset;
        contentsList_.value().children().append(
            listItem(entry.title, {}, std::min<float>(entry.level, 4) * 14.0f,
                     [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::fillBookmarks() {
    bookmarkList_.value().children().clear();

    if (!state_ || state_->bookmarks.empty()) {
        bookmarkNote_.value().text(L"Закладок пока нет.");
        return;
    }

    bookmarkNote_.value().text({});

    for (const Bookmark& mark : state_->bookmarks) {
        const std::uint32_t offset = mark.charOffset;
        bookmarkList_.value().children().append(
            listItem(mark.hint.empty() ? L"Закладка" : mark.hint, {}, 0,
                     [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::runSearch() {
    searchList_.value().children().clear();

    const std::wstring needle{reinterpret_cast<wchar_t const*>(searchBox_.value().text().c_str())};
    if (needle.empty() || blank(needle)) {
        searchNote_.value().text(L"Введите слово и нажмите Enter.");
        return;
    }

    const std::vector<SearchHit> hits = searchBook(view_.blocks(), needle);
    if (hits.empty()) {
        searchNote_.value().text(std::format(L"«{}» в книге не нашлось.", needle));
        return;
    }

    searchNote_.value().text(std::format(L"Нашлось: {}", hits.size()));

    for (const SearchHit& hit : hits) {
        const std::uint32_t offset = hit.charOffset;
        searchList_.value().children().append(
            listItem(hit.context, {}, 0, [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::toggleBookmark() {
    if (!state_ || !view_.isOpen()) return;

    const std::uint32_t here = view_.readingPosition();

    const auto found = std::find_if(state_->bookmarks.begin(), state_->bookmarks.end(),
                                    [here](const Bookmark& mark) {
                                        return mark.charOffset == here;
                                    });
    if (found != state_->bookmarks.end()) {
        state_->bookmarks.erase(found);
    } else {
        // Закладки лежат по порядку книги: так их и читают, и так список не
        // приходится сортировать при показе.
        Bookmark mark{here, hintAt(view_.blocks(), here)};
        const auto after = std::lower_bound(state_->bookmarks.begin(), state_->bookmarks.end(),
                                            here, [](const Bookmark& mark, std::uint32_t offset) {
                                                return mark.charOffset < offset;
                                            });
        state_->bookmarks.insert(after, std::move(mark));
    }

    fillBookmarks();
    if (onStateChanged) onStateChanged();
}

void ReaderPanel::refreshThemes() {
    using namespace wxl::dsl;

    themesPanel_.value().children().clear();
    themeButtons_.clear();

    // Индексы тем сквозные: сперва встроенные, затем обложки — ровно так их
    // считает и полоса набора. markTheme() ходит по кнопкам тем же счётом.
    auto themeButton = [this](std::wstring_view caption, int index, Thickness margin) {
        return Button{
            caption,
            fontSize = 13,
            Margin{margin},
            Padding{12, 6},
            foreground = SolidColorBrush{ARGB{kInk}},
            background = SolidColorBrush{ARGB{0x00000000}},
            borderBrush = SolidColorBrush{ARGB{kEdge}},
            BorderThickness{1},
            CornerRadius{4},
            onClick =
                [this, index](Object const&, RoutedEventArgs&) {
                    view_.setTheme(index);
                    markTheme();
                    if (onSettingsChanged) onSettingsChanged();
                },
        };
    };

    // Встроенные — в строчку, как и были: их четыре, и они короткие.
    auto builtins = StackPanel{Orientation::Horizontal};
    for (int index = 0; index < kThemeCount; ++index) {
        auto button = themeButton(kThemes[index].name, index, Thickness{0, 0, 6, 0});
        themeButtons_.push_back(button);
        builtins.children().append(button);
    }
    themesPanel_.value().children().append(builtins);

    // Обложки — по строке на каждую: имя даёт читатель, и в строчку они не
    // помещаются. Рядом с каждой — шестерёнка: обложку не только выбирают,
    // но и правят, и дорога к правке стоит у самой обложки.
    const std::vector<Skin>& skins = view_.skins();
    for (std::size_t index = 0; index < skins.size(); ++index) {
        auto button = themeButton(skins[index].name, kThemeCount + static_cast<int>(index),
                                  Thickness{0, 6, 0, 0});
        themeButtons_.push_back(button);

        // Кнопка без текста обязана иметь тултип (правило дизайна, см.
        // CLAUDE.md) — и он называет конкретную обложку, а не действие
        // вообще.
        const std::wstring tip = L"Настроить подложку «" + skins[index].name + L"»";

        auto gear = Button{
            L"",   // шестерёнка Segoe Fluent Icons
            fontFamily = FontFamily{L"Segoe Fluent Icons"},
            toolTip = tip.c_str(),
            fontSize = 13,
            Margin{6, 6, 0, 0},
            Padding{8, 6},
            foreground = SolidColorBrush{ARGB{kDim}},
            background = SolidColorBrush{ARGB{0x00000000}},
            borderBrush = SolidColorBrush{ARGB{kEdge}},
            BorderThickness{1},
            CornerRadius{4},
            onClick =
                [this, name = skins[index].name](Object const&, RoutedEventArgs&) {
                    if (onEditSkin) onEditSkin(name);
                },
        };

        themesPanel_.value().children().append(StackPanel{
            Orientation::Horizontal,
            button,
            gear,
        });
    }

    // Дорога в мастер — последней строкой, после всех тем.
    themesPanel_.value().children().append(Button{
        L"Добавить обложку…",
        fontSize = 13,
        Margin{0, 6, 0, 0},
        Padding{12, 6},
        foreground = SolidColorBrush{ARGB{kDim}},
        background = SolidColorBrush{ARGB{0x00000000}},
        borderBrush = SolidColorBrush{ARGB{kEdge}},
        BorderThickness{1},
        CornerRadius{4},
        onClick =
            [this](Object const&, RoutedEventArgs&) {
                if (onAddSkin) onAddSkin();
            },
    });

    markTheme();
}

void ReaderPanel::markTheme() {
    // Тема — выбор из трёх, а выбор видно только тогда, когда выбранное
    // отмечено. Отмечается тем же цветом, что и открытая вкладка: одна
    // и та же мысль — «вот это сейчас».
    for (int index = 0; index < static_cast<int>(themeButtons_.size()); ++index) {
        themeButtons_[static_cast<std::size_t>(index)].background(
            SolidColorBrush{ARGB{index == view_.theme() ? kActive : 0x00000000u}});
    }
}

void ReaderPanel::syncSettings() {
    // Ползунки ставятся из текущего вида, и их же событие тут же прилетит
    // обратно; флаг говорит обработчику, что это мы, а не читатель.
    filling_ = true;
    fontSize_.value().value(view_.fontSize());
    lineHeight_.value().value(view_.lineHeight() * 100.0);
    margin_.value().value(view_.margin() * 100.0);
    filling_ = false;

    // Тему меняют ещё и клавишей T мимо панели, поэтому отметка ставится при
    // каждом открытии вкладки, а не только по нажатию здешней кнопки.
    markTheme();
}

}  // namespace bukvitsa::reader
