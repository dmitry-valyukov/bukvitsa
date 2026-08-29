#include "start_screen.h"

#include <chrono>

#include "card.h"

namespace bukvitsa::reader {

using namespace wxl;
using namespace wxl::dsl;
using namespace std::chrono_literals;

namespace {

// Ширина колонки кнопок; где ей стоять, знает карточка (card.h).
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

// Откуда кнопка приезжает. Одной прозрачности мало — появление «из ничего»
// читается плоско, а десяток пикселей вверх делает его живым.
constexpr Vector3 kRiseFrom{0.0f, 14.0f, 0.0f};

}  // namespace

StartScreen::StartScreen(const Compositor& compositor) : compositor_(compositor) {
    // Кнопки собираются раньше корня: каждая должна успеть отдать свой визуал
    // в revealing_ до того, как дерево уедет в конструктор Grid.
    auto panel = StackPanel{
        addButton(L"Продолжить чтение", 72.0f, 19.0f, &onContinueReading),
        addButton(L"Моя библиотека", 46.0f, 15.0f, &onLibrary),
        addButton(L"Добавить книгу", 46.0f, 15.0f, &onAddBook),
        addButton(L"Добавить каталог", 46.0f, 15.0f, &onAddFolder),
        addButton(L"Выйти из читалки", 46.0f, 15.0f, &onExit, true),
    };

    // Кнопки лежат на карточке — той же, что у мастера обложек. Проступать
    // ей вместе с ними, поэтому прозрачность в ноль сразу, при построении.
    auto card = buttonCard(panel);
    Visual cardVisual = ElementCompositionPreview::getElementVisual(card);
    cardVisual.opacity(0.0f);
    cardVisual_ = cardVisual;

    // Картинка и панель — дети одной ячейки Grid: порядок объявления и есть
    // порядок по глубине, так что панель ложится поверх заставки.
    //
    // stretch назван по имени: тег этого свойства выпущен без типа (одно имя,
    // разные типы у разных классов), а значит безымянного маршрута у него нет.
    //
    // UniformToFill, а не Fill: пропорции заставки сохраняются, лишнее
    // срезается краями окна. Растянутая по обеим осям картинка выдаёт себя
    // сразу, какой бы формы ни было окно.
    root_ = Grid{
        // Корень берёт фокус на себя, иначе клавиатура не работает вовсе:
        // событие клавиши начинается у того, на чём фокус, и пока фокуса нет
        // ни на чём, ловить нечего — ни на всплытии, ни на пути вниз.
        isTabStop = true,
        Image{
            source = L"Assets/splash-screen-1k.png",
            stretch = Stretch::UniformToFill,
            hAlign.center,
            vAlign.top,
        },
        card,
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
