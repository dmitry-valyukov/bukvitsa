#include "library_screen.h"

#include <format>

namespace bukvitsa::reader {

using namespace wxl;
using namespace wxl::dsl;

namespace {

// Полка бумажного цвета, как и полоса набора: витрина — часть той же книги,
// а не отдельное приложение.
constexpr uint32_t kPaper = 0xFFF7F4EE;
constexpr uint32_t kInk = 0xFF201E1C;
constexpr uint32_t kDim = 0xFF8A857D;
constexpr uint32_t kCard = 0xFFFFFDF9;
constexpr uint32_t kEdge = 0xFFE3DED4;

// Обложка стоит в пропорции 2:3 — так их печатают, и так они не прыгают по
// высоте, когда у одной книги обложка квадратная, а у другой узкая.
constexpr double kCoverWidth = 72;
constexpr double kCoverHeight = 108;

/// Путь к обложке как источник картинки. Путь абсолютный, поэтому со схемой:
/// без схемы wxl разрешает его рядом с исполняемым файлом.
ImageSource coverOf(const BookEntry& entry) {
    if (entry.cover.empty()) return {};

    // Имя нарочно не text: одноимённый тег синтаксиса перекрылся бы им.
    std::wstring full = (coverDirectory() / entry.cover).wstring();
    std::replace(full.begin(), full.end(), L'\\', L'/');
    return ImageSource{L"file:///" + full};
}

/// Сколько прочитано, словами витрины. Место чтения лежит в отдельном файле на
/// книгу, и читается оно здесь — то есть только когда полку показывают.
std::wstring progressOf(const BookEntry& entry) {
    if (entry.characterCount == 0) return L"не открывалась";

    const BookState state = loadBookState(entry.guid);
    if (state.charOffset == 0) return L"не открывалась";

    const double share = static_cast<double>(state.charOffset) / entry.characterCount;
    std::wstring said = std::format(L"прочитано {:.0f}%", share * 100.0);

    // Закладки видно прямо на полке: их наличие -- признак книги, к которой
    // возвращаются, и его стоит показать раньше, чем её откроют.
    if (!state.bookmarks.empty()) {
        said += std::format(L"    закладок: {}", state.bookmarks.size());
    }
    return said;
}

}  // namespace

LibraryScreen::LibraryScreen() {
    shelf_ = StackPanel{Margin{40, 8, 40, 32}};

    emptyNote_ = TextBlock{
        L"Пока пусто. Добавьте книгу — она останется там, где лежит.",
        fontSize = 16,
        foreground = SolidColorBrush{ARGB{kDim}},
        Margin{40, 24, 40, 0},
    };

    continueBox_ = CheckBox{
        L"Продолжать чтение при старте",
        column = 1,
        foreground = SolidColorBrush{ARGB{kInk}},
        vAlign.center,
    };

    // Обе стороны одного вопроса: галочку и снимают, и ставят, а слушателю
    // важно только новое значение.
    auto const told = [this](Object const&, RoutedEventArgs&) {
        if (onContinueAtStartChanged) {
            onContinueAtStartChanged(continueBox_.value().isChecked().value_or(false));
        }
    };
    continueBox_.value().add_onChecked(told);
    continueBox_.value().add_onUnchecked(told);

    root_ = Grid{
        isTabStop = true,
        background = SolidColorBrush{ARGB{kPaper}},
        rowDefinitions = L"auto,*",

        Grid{
            row = 0,
            Margin{40, 32, 40, 8},
            columnDefinitions = L"*,auto,auto,auto",
            columnSpacing = 12,

            TextBlock{
                L"Моя библиотека",
                column = 0,
                fontSize = 26,
                FontWeight{600},
                foreground = SolidColorBrush{ARGB{kInk}},
                vAlign.center,
            },
            // Место в сетке задано при постройке, вместе со всем остальным:
            // тег колонки живёт на самом элементе, а не на сетке.
            continueBox_.value(),
            Button{
                L"Добавить книгу",
                column = 2,
                onClick = [this](Object const&,
                                 RoutedEventArgs&) { if (onAddBook) onAddBook(); },
            },
            Button{
                L"Назад",
                column = 3,
                onClick = [this](Object const&, RoutedEventArgs&) { if (onBack) onBack(); },
            },
        },

        ScrollViewer{
            row = 1,
            content = StackPanel{
                emptyNote_.value(),
                shelf_.value(),
            },
        },
    };

    root_.value().add_onLoaded([this](Object const&, RoutedEventArgs&) {
        root_.value().focus(FocusState::Programmatic);
    });
}

void LibraryScreen::show(const Library& library, bool continueAtStart) {
    continueBox_.value().isChecked(continueAtStart);

    // Полка пересобирается целиком. Сравнивать её с реестром и править
    // разницу было бы дороже во всех смыслах: книг десятки, а не тысячи, и
    // добавление одной — не повод заводить вторую модель того же списка.
    shelf_.value().children().clear();
    for (const BookEntry& entry : library.books()) {
        shelf_.value().children().append(shelfItem(entry));
    }

    emptyNote_.value().visibility(library.books().empty() ? Visibility::Visible
                                                         : Visibility::Collapsed);
}

Button LibraryScreen::shelfItem(const BookEntry& entry) {
    // Карточка — это кнопка: по книге щёлкают, и всё, что кнопка умеет сама
    // (наведение, нажатие, фокус, клавиатура), достаётся даром.
    std::wstring const guid = entry.guid;

    return Button{
        hAlign.stretch,
        // Содержимое кнопки по умолчанию стоит по центру -- для карточки это
        // значит текст посреди пустоты. Растянуть его надо явно, и это
        // horizontalContentAlignment, а не hAlign: тот про саму кнопку.
        horizontalContentAlignment = HorizontalAlignment::Stretch,
        Margin{0, 6},
        Padding{0},
        background = SolidColorBrush{ARGB{kCard}},
        borderBrush = SolidColorBrush{ARGB{kEdge}},
        BorderThickness{1},
        CornerRadius{6},
        onClick = [this, guid](Object const&,
                               RoutedEventArgs&) { if (onOpen) onOpen(guid); },

        content = Grid{
            columnDefinitions = L"auto,*",
            columnSpacing = 16,
            Margin{12},

            Image{
                column = 0,
                source = coverOf(entry),
                width = kCoverWidth,
                height = kCoverHeight,
                stretch = Stretch::UniformToFill,
                vAlign.top,
            },

            StackPanel{
                column = 1,
                vAlign.center,
                TextBlock{
                    entry.title,
                    fontSize = 18,
                    FontWeight{600},
                    foreground = SolidColorBrush{ARGB{kInk}},
                    textWrapping.wrap,
                    maxLines = 2,
                    textTrimming.characterEllipsis,
                },
                TextBlock{
                    entry.authors,
                    fontSize = 14,
                    foreground = SolidColorBrush{ARGB{kDim}},
                    Margin{0, 4, 0, 0},
                    maxLines = 1,
                    textTrimming.characterEllipsis,
                },
                TextBlock{
                    progressOf(entry),
                    fontSize = 13,
                    foreground = SolidColorBrush{ARGB{kDim}},
                    Margin{0, 8, 0, 0},
                },
            },
        },
    };
}

}  // namespace bukvitsa::reader
