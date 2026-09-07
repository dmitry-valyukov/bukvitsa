#include "start_screen.h"

#include <algorithm>
#include <chrono>


namespace bukvitsa::reader {

using namespace wxl;
using namespace wxl::dsl;
using namespace std::chrono_literals;

namespace {

// Ширина колонки кнопок; где ей стоять, сказано у самой карточки.
constexpr float kButtonWidth = 300.0f;

// Проявление: длительность одной кнопки и разбег между соседними. Четыре
// кнопки с шагом 40 мс успокаиваются к 320 мс — за 400 мс появление уже
// читается как задержка, так что расти этому некуда: если кнопок станет
// больше, уменьшать надо шаг, а не растягивать целое.
constexpr auto kFadeDuration = 220ms;
constexpr auto kStagger = 40ms;

// Кнопки полупрозрачные: под ними картинка, и она должна просвечивать.
constexpr float kRestingOpacity = 0.92f;

// Лицо кнопки отмены: чуть серее остальных — она уводит, а не ведёт.
constexpr uint32_t kCancelFace = 0xFFD9D6D2;

// Большая кнопка, когда ей есть что продолжать: высота под обложку, обложка
// в пропорции витрины, автор — тем же приглушённым тоном, что и там.
constexpr float kContinueTall = 100.0f;
constexpr double kCoverTall = 76.0;
constexpr uint32_t kDimInk = 0xFF8A857D;

// Откуда кнопка приезжает. Одной прозрачности мало — появление «из ничего»
// читается плоско, а десяток пикселей вверх делает его живым.
constexpr Vector3 kRiseFrom{0.0f, 14.0f, 0.0f};

}  // namespace

StartScreen::StartScreen(const Compositor& compositor) : compositor_(compositor) {
    // Кнопки собираются раньше корня: каждая должна успеть отдать свой визуал
    // в revealing_ до того, как дерево уедет в конструктор Grid. Большая
    // кнопка остаётся в руках: setContinueBook() наполнит её книгой.
    auto continueButton = addButton(L"Продолжить чтение", 72.0f, 19.0f, &onContinueReading);
    continueButton_ = continueButton;

    auto panel = StackPanel{
        continueButton,
        addButton(L"Моя библиотека", 46.0f, 15.0f, &onLibrary),
        addButton(L"Добавить книгу", 46.0f, 15.0f, &onAddBook),
        addButton(L"Добавить каталог", 46.0f, 15.0f, &onAddFolder),
        addButton(L"Выйти из читалки", 46.0f, 15.0f, &onExit, true),
    };

    // Кнопки лежат на карточке — той же, что у мастера обложек. Проступать
    // ей вместе с ними, но гасить прозрачность самой карточки нельзя: у неё
    // заняты фасадные свойства (translation несёт тень), а трогать
    // handout-визуал элемента с фасадами запрещено — XAML тогда прикладывает
    // смещение карточки к попаданию мыши дважды, кнопки рисуются на месте, а
    // ловят щелчки за правым краем экрана (проверено UIA: x кнопок удвоился).
    // Поэтому прозрачностью проявляется обёртка, у которой фасадов нет.
    //
    // Сама карточка — библиотечная wxl::OverlayCard, та, что для страницы с
    // картинкой под ней. Своего здесь только место.
    auto card = Built<OverlayCard>{
        hAlign.right,
        vAlign.top,
        Margin{0, 64, 72, 0},
        panel,
    };
    auto cardShell = Grid{card};
    Visual cardVisual = ElementCompositionPreview::getElementVisual(cardShell);
    cardVisual.opacity(0.0f);
    cardVisual_ = cardVisual;

    // Заставки в этом дереве нет. Задняя картинка окна ровно одна — задник
    // сцены, который ставит main.cpp (window->backgroundAsync); остров
    // прозрачен, и сквозь него видна она. Второй вывод той же картинки
    // XAML-элементом Image был бы дублем, а дублей быть не должно: остров
    // несёт только карточку с кнопками.
    root_ = Grid{
        // Корень берёт фокус на себя, иначе клавиатура не работает вовсе:
        // событие клавиши начинается у того, на чём фокус, и пока фокуса нет
        // ни на чём, ловить нечего — ни на всплытии, ни на пути вниз.
        isTabStop = true,
        cardShell,
    };

    // Просить фокус раньше, чем дерево живо, бесполезно: элемент вне
    // визуального дерева тихо отказывает.
    root_.value().add_onLoaded([this](Object const&, RoutedEventArgs&) {
        root_.value().focus(FocusState::Programmatic);
    });

    // Enter — действие по умолчанию, то же, что большая кнопка; Escape —
    // отмена, то же, что «Выйти из читалки». На пути вниз, чтобы клавиша
    // работала независимо от того, на какой кнопке стоит фокус.
    root_.value().add_onPreviewKeyDown([this](Object const&, KeyRoutedEventArgs& args) {
        switch (args.key()) {
            case VirtualKey::Enter:
                if (onContinueReading) onContinueReading();
                break;
            case VirtualKey::Escape:
                if (onExit) onExit();
                break;
            default: return;
        }
        args.handled(true);
    });
}

Button StartScreen::addButton(std::wstring_view caption, float tall, float kegel,
                              // Имена нарочно не height, не fontSize и не text:
                              // параметр с именем свойства перекрыл бы одноимённый
                              // тег DSL, и `height = height` стало бы
                              // присваиванием float.
                              std::function<void()>* action, bool cancel) {
    auto button = Button{
        caption,
        width = kButtonWidth,
        height = tall,
        FontWeight{600},
        Margin{0, 6},
        fontSize = kegel,
        hAlign.stretch,
        onClick =
            [action](Object const&, RoutedEventArgs&) {
                if (*action) (*action)();
            },
    };

    if (cancel) button.background(SolidColorBrush{ARGB{kCancelFace}});

    // Подъём идёт по Translation, а НЕ по Offset. Offset — это то, чем XAML
    // расставляет элементы при разметке: анимация захватывает свойство себе,
    // и все четыре кнопки съезжаются в начало панели друг на друга. Проверено
    // на себе. Translation — отдельное свойство поверх разметки, и живёт оно
    // только после setIsTranslationEnabled, а до первой вставки в набор
    // свойств его вообще нет.
    ElementCompositionPreview::setIsTranslationEnabled(button, true);

    Visual visual = ElementCompositionPreview::getElementVisual(button);

    // Прозрачность и подъём ставятся здесь, а не в reveal(): между
    // построением дерева и готовностью приложения проходит кадр, и на нём
    // кнопка успела бы мигнуть во всю силу.
    //
    // Смещение тоже сразу — во время задержки анимация ещё не трогает
    // свойство, и кнопка со штатным смещением просто стояла бы на месте, а
    // потом прыгнула.
    visual.opacity(0.0f);
    visual.properties().insertVector3(L"Translation", kRiseFrom);
    revealing_.push_back(visual);

    return button;
}

void StartScreen::setContinueBook(std::wstring_view title, std::wstring_view author,
                                  const std::filesystem::path& cover) {
    if (title.empty()) return;   // продолжать нечего — кнопка остаётся простой надписью

    // Сюда попадают на каждом показе экрана, а книга меняется редко:
    // перестраивать то же самое незачем.
    std::wstring key = std::wstring(title) + L'\n' + std::wstring(author) + L'\n' + cover.wstring();
    if (key == continueKey_) return;
    continueKey_ = std::move(key);

    // Колонка текста: своя надпись кнопки, под ней название, под ним автор.
    // Grid со звёздной колонкой, а не горизонтальный StackPanel: тот мерил бы
    // текст бесконечной шириной, и длинному названию не с чего было бы
    // обрезаться.
    auto lines = StackPanel{
        column = 1,
        vAlign.center,
        TextBlock{L"Продолжить чтение", fontSize = 19, FontWeight{600}},
        TextBlock{std::wstring(title), fontSize = 13, Margin{0, 5, 0, 0},
                  textTrimming.characterEllipsis},
        TextBlock{std::wstring(author), fontSize = 12, Margin{0, 2, 0, 0},
                  foreground = SolidColorBrush{ARGB{kDimInk}}, textTrimming.characterEllipsis},
    };

    Button button = continueButton_.value();

    if (cover.empty()) {
        button.content(Grid{lines});
    } else {
        // Путь абсолютный, поэтому со схемой: без неё wxl искал бы картинку
        // рядом с исполняемым файлом — так же устроена обложка на витрине.
        std::wstring full = cover.wstring();
        std::replace(full.begin(), full.end(), L'\\', L'/');

        button.content(Grid{
            columnDefinitions = L"auto,*",
            Image{
                source = ImageSource{L"file:///" + full},
                height = kCoverTall,
                Margin{0, 0, 12, 0},
            },
            lines,
        });
    }

    // Растянуть, а не влево: при выравнивании по левому краю содержимому
    // отдали бы его желанную ширину, и обрезание длинного названия не
    // сработало бы.
    button.height(kContinueTall);
    button.horizontalContentAlignment(HorizontalAlignment::Stretch);
}

void StartScreen::reveal() {
    if (revealed_) return;   // окно может стать видимым не один раз
    revealed_ = true;

    auto const easing = compositor_.createLinearEasingFunction();

    // Карточка — только прозрачностью и без разбега: подъём по Z несёт её
    // тень, и анимация Translation увела бы его в ноль.
    if (cardVisual_) {
        auto fade = compositor_.createScalarKeyFrameAnimation();
        fade.duration(kFadeDuration);
        fade.insertKeyFrame(1.0f, 1.0f, easing);
        cardVisual_.value().startAnimation(L"Opacity", fade);
    }

    for (std::size_t index = 0; index < revealing_.size(); ++index) {
        auto const delay = kStagger * static_cast<int>(index);
        const Visual& visual = revealing_[index];

        auto fade = compositor_.createScalarKeyFrameAnimation();
        fade.duration(kFadeDuration);
        fade.delayTime(delay);
        fade.insertKeyFrame(1.0f, kRestingOpacity, easing);

        auto rise = compositor_.createVector3KeyFrameAnimation();
        rise.duration(kFadeDuration);
        rise.delayTime(delay);
        rise.insertKeyFrame(1.0f, Vector3{0.0f, 0.0f, 0.0f}, easing);

        // Анимация крутится на потоке DWM: пока кнопки проступают, поток
        // приложения свободен — и будет чем занять его, когда за кнопками
        // появится реестр книг.
        visual.startAnimation(L"Opacity", fade);
        visual.startAnimation(L"Translation", rise);
    }
}

}  // namespace bukvitsa::reader
