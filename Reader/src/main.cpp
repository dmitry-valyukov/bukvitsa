// Reader — читалка Буквицы на wxl.winui.
//
// Здесь нет ни wWinMain, ни поднятия Windows App Runtime, ни наследника
// Application, ни XAML: всё это делает wxl и потом зовёт эту функцию.
//
// Что здесь есть — сборка приложения из частей и решения о том, что чем
// сменяется: окно с родной рамкой и запомненным местом, стартовый экран, витрина
// хранилища, полоса набора, реестр книг и полный экран по F11. Чего ещё нет
// и что идёт следующим — в docs/reader.md.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core: заголовок, включённый после импорта, MSVC уже не принимает.
#include <windows.h>

#include "file_dialog.h"
#include "library.h"
#include "settings.h"

#include "FileDrop.h"
#include "WindowHandle.h"
#include "WindowPlacement.h"
#include "library_screen.h"
#include "start_screen.h"

// Последним: он ведёт к модели книги, а она импортирует wxl.text, после чего
// стандартный заголовок MSVC уже не принимает.
#include "book.h"
#include "book_view.h"
#include "reader_panel.h"
#include "store.h"

// Импорт последним, после всех обычных заголовков.
import wxl.text;

using namespace wxl;
using namespace wxl::dsl;
using namespace std::chrono_literals;

namespace {

using namespace bukvitsa::reader;

// Каким окно открывается, когда запоминать ещё нечего.
constexpr int32_t kInitialWidth = 1280;
constexpr int32_t kInitialHeight = 860;

// Сколько заставка стоит одна, прежде чем на неё проступят кнопки. Не таймаут
// загрузки, а пауза ради самой заставки: приложению без книги нечего грузить,
// и без неё кнопки появились бы в тот же кадр, что и картинка.
constexpr auto kSplashHold = 1000ms;

// Пауза, после которой перемещение окна попадает на диск. Окно таскают
// непрерывно, а писать на каждый пиксель незачем; при закрытии сохранение
// безусловное, так что пауза ничего не теряет — она только страхует от того,
// что до закрытия дело не дойдёт.
constexpr auto kSaveQuiet = 800ms;

// То же для места чтения: страницы листают подряд, а файл на книгу один.
constexpr auto kPositionQuiet = 1500ms;

// Что сейчас в окне. Три экрана, и переход между ними — присваивание
// содержимого; перечисление нужно только затем, чтобы Escape знал, куда
// возвращать.
enum class Screen { Start, Library, Book };

}  // namespace

wxl::Teardown wxl_launched() {
    auto settings = std::make_shared<Settings>(loadSettings());
    auto library = std::make_shared<Library>();
    library->load();

    auto window = Window{
        title = L"Буквица",
        minSize = {720, 520},
    };

    auto const appWindow = window.appWindow();

    auto screen = std::make_shared<StartScreen>(window.compositor());
    auto view = std::make_shared<BookView>(window.compositor(), window.dispatcherQueue());
    auto shelf = std::make_shared<LibraryScreen>();

    // Панель живёт поверх полосы набора: «поверх страницы» — это внутри полосы,
    // а не рядом с ней.
    auto panel = std::make_shared<ReaderPanel>(window.compositor(), *view);
    view->addOverlay(panel->root());

    // Состояние открытой книги: место чтения и закладки. Читается и пишется
    // целиком, потому что файл переписывается заменой — «дописать одно поле»
    // всё равно значит написать его весь.
    auto state = std::make_shared<BookState>();

    // Настройки чтения общие для всех книг: читателю нужен один привычный вид,
    // а не разный шрифт в каждой книге.
    view->setTheme(settings->theme);
    view->setFontSize(settings->fontSize);
    view->setLineHeight(settings->lineHeight);
    view->setMargin(settings->margin);

    // ---- сохранение места чтения ----
    //
    // Отложенно, как и место окна: перелистывание — самое частое действие в
    // читалке, а файл состояния книги переписывается целиком.
    auto positionTimer = window.dispatcherQueue().createTimer();
    positionTimer.interval(kPositionQuiet);
    positionTimer.isRepeating(false);

    auto const rememberPosition = [view, settings, state] {
        // Книга закрыта -- писать нечего: место чтения принадлежит ей, а не
        // окну, и ноль незанятой полосы стёр бы то, что уже записано.
        if (settings->lastBookGuid.empty() || !view->isOpen()) return;
        state->charOffset = view->readingPosition();
        saveBookState(settings->lastBookGuid, *state);
    };

    positionTimer.add_onTick([positionTimer, rememberPosition](Object const&, Object const&) {
        positionTimer.stop();
        rememberPosition();
    });

    view->onPositionChanged = [positionTimer](std::uint32_t) {
        positionTimer.stop();
        positionTimer.start();
    };

    // ---- смена содержимого окна ----
    //
    // Три экрана — заставка, витрина и полоса набора — это три содержимого
    // одного окна, и переключение между ними одно присваивание. Второго окна
    // нет намеренно: оно завело бы вторую кнопку на панели задач.
    //
    // Куда возвращает Escape, знает `shown`, а `bookCameFrom` помнит, откуда
    // книгу открыли: читатель, выбравший её на полке, ждёт полку обратно, а
    // не заставку.
    auto shown = std::make_shared<Screen>(Screen::Start);
    auto bookCameFrom = std::make_shared<Screen>(Screen::Start);

    auto const closePanel = [panel] { panel->close(); };

    auto const showStartScreen = [window, screen, shown, rememberPosition, closePanel] {
        closePanel();
        // Уходя из книги, место чтения пишем сразу: отложенная запись ждёт
        // паузы, а читатель уже ушёл -- и, может быть, закроет приложение
        // раньше, чем таймер сработает.
        rememberPosition();
        *shown = Screen::Start;
        window.content(screen->root());
    };

    auto const showLibrary = [window, shelf, library, settings, shown, rememberPosition,
                              closePanel] {
        closePanel();
        rememberPosition();   // и полка тут же покажет свежий процент
        // Полка пересобирается на каждый показ: книга могла добавиться, а
        // место чтения — уехать с тех пор, как её видели в прошлый раз.
        shelf->show(*library, settings->continueReading);
        *shown = Screen::Library;
        window.content(shelf->root());
    };

    auto const openBook = [window, view, panel, library, settings, state, shown, bookCameFrom,
                           rememberPosition](std::filesystem::path const& path) {
        std::shared_ptr<Book> book;
        try {
            book = std::make_shared<Book>(path, dwriteFactory());
        } catch (std::exception const& failure) {
            // Разговор с читателем, а не запись в лог: он только что выбрал
            // этот файл и вправе узнать, что с ним не так.
            wxl::text::u16_text const reason = wxl::text::assume_valid(failure.what()).to_utf16();
            std::wstring const complaint = L"Не удалось открыть книгу:\n" + path.wstring() + L"\n\n" +
                                           std::wstring(reason.wchars());
            ::MessageBoxW(window_handle(window), complaint.c_str(), L"Буквица",
                          MB_OK | MB_ICONWARNING);
            return;
        }

        // Место чтения предыдущей книги — на диск сразу: сейчас settings
        // укажет на другую, и записывать станет некуда.
        rememberPosition();

        BookEntry const& stored = library->add(*book);
        library->save();

        settings->lastBookGuid = stored.guid;
        settings->lastBookPath = stored.path;
        saveSettings(*settings);

        *state = loadBookState(stored.guid);
        view->open(std::move(book), state->charOffset);
        panel->setState(state.get());

        // Сверстать и нарисовать до показа. Полоса займёт то же место, что и
        // экран, который сейчас на нём стоит, — а у него и спрашиваем размер с
        // масштабом. Иначе читатель, нажав «Продолжить чтение», успевает
        // увидеть пустой лист: элемент попадает в дерево сразу, а рисовать его
        // есть чем только со следующего кадра.
        if (Nullable<UIElement> const showing = window.content()) {
            if (Nullable<XamlRoot> const root = showing->xamlRoot()) {
                Size const area = root->size();
                view->prepare(area.width, area.height,
                              static_cast<float>(root->rasterizationScale()));
            }
        }

        *bookCameFrom = *shown;
        *shown = Screen::Book;
        window.content(view->root());
    };

    auto const addBook = [window, openBook] {
        std::filesystem::path const path = askForBook(window_handle(window));
        if (!path.empty()) openBook(path);
    };

    screen->onAddBook = addBook;
    screen->onAddFolder = [] {};   // наблюдаемый каталог — следующая итерация
    screen->onLibrary = showLibrary;

    shelf->onAddBook = addBook;
    shelf->onBack = showStartScreen;
    shelf->onOpen = [library, openBook](std::wstring guid) {
        if (BookEntry const* entry = library->find(guid)) openBook(entry->path);
    };
    shelf->onContinueAtStartChanged = [settings](bool wanted) {
        settings->continueReading = wanted;
        saveSettings(*settings);
    };

    panel->onStateChanged = [settings, state] { saveBookState(settings->lastBookGuid, *state); };

    panel->onLibrary = showLibrary;

    // Правая кнопка по странице открывает ящик. Другой дороги к нему у мыши
    // нет: у страницы книги нет ни полосы меню, ни кнопок — и не должно быть.
    view->onPanelRequested = [panel] {
        if (panel->isOpen()) {
            panel->close();
        } else {
            panel->open(ReaderPanel::Tab::Contents);
        }
    };

    panel->onSettingsChanged = [settings, view] {
        settings->theme = view->theme();
        settings->fontSize = view->fontSize();
        settings->lineHeight = view->lineHeight();
        settings->margin = view->margin();
        saveSettings(*settings);
    };

    screen->onContinueReading = [settings, library, openBook, addBook] {
        // Путь в настройках — копия того, что в реестре, и она здесь ради
        // быстрого пути. Протухла — спрашиваем реестр по guid; нет и там —
        // читателю нечего продолжать, и он хотел открыть книгу.
        std::wstring path = settings->lastBookPath;
        if (path.empty() || !std::filesystem::exists(path)) {
            if (BookEntry const* entry = library->find(settings->lastBookGuid)) {
                path = entry->path;
            }
        }
        if (path.empty() || !std::filesystem::exists(path)) {
            addBook();
            return;
        }
        openBook(path);
    };

    // ---- клавиши, общие для обоих экранов ----
    //
    // Полный экран по F11, выход из него по Escape. Перехват на пути вниз, а
    // не на всплытии: событие начинается у того, на чём фокус, и клавиша
    // должна работать независимо от того, на какой кнопке он сейчас стоит.
    //
    // Escape решается здесь, а не в полосе набора: приоритет один на всё
    // приложение — сперва закрыть открытое поверх, потом выйти из полного
    // экрана, и лишь потом вернуться из книги на стартовый экран.
    // Флага «мы в полном экране» нет намеренно: он был бы вторым местом, где
    // это записано, и разошёлся бы с первым в тот же миг, когда presenter
    // сменил кто-то другой — например, сама wxl, восстанавливая окно, которое
    // закрыли полноэкранным.
    auto const isFullScreen = [appWindow] {
        return appWindow.presenter().kind() == AppWindowPresenterKind::FullScreen;
    };

    auto const setFullScreen = [appWindow](bool on) {
        appWindow.setPresenter(on ? AppWindowPresenterKind::FullScreen
                                  : AppWindowPresenterKind::Overlapped);
    };

    auto const installKeys = [setFullScreen, isFullScreen, shown, bookCameFrom, showStartScreen,
                              showLibrary, panel, view](UIElement const& element) {
        element.add_onPreviewKeyDown([=](Object const&, KeyRoutedEventArgs& args) {
            if (args.handled()) return;   // полоса набора своё уже разобрала

            // Панель — только над книгой: над заставкой ей нечего показывать.
            bool const reading = *shown == Screen::Book;
            bool const control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

            // Полка — отовсюду, а не только из книги: выбрать другую книгу
            // читатель вправе в любой момент, и это единственная клавиша,
            // которой не мешает то, что сейчас на экране.
            if (control && args.key() == VirtualKey::L) {
                if (*shown != Screen::Library) showLibrary();
                args.handled(true);
                return;
            }

            if (reading && control) {
                switch (args.key()) {
                    case VirtualKey::T: panel->open(ReaderPanel::Tab::Contents); break;
                    case VirtualKey::F: panel->open(ReaderPanel::Tab::Search); break;
                    case VirtualKey::B: panel->open(ReaderPanel::Tab::Bookmarks); break;
                    default:
                        // Ctrl с чем-то другим — не наше: кегль и тему разбирает
                        // сама полоса, до сюда они не доходят.
                        return;
                }
                args.handled(true);
                return;
            }

            switch (args.key()) {
                case VirtualKey::F2:
                    if (!reading) return;
                    if (panel->isOpen()) {
                        panel->close();
                    } else {
                        panel->open(ReaderPanel::Tab::Settings);
                    }
                    break;
                case VirtualKey::F11:
                    setFullScreen(!isFullScreen());
                    break;
                case VirtualKey::Escape:
                    // Один приоритет на всё приложение: сперва убрать то, что
                    // лежит поверх страницы, потом выйти из полного экрана, и
                    // лишь потом уйти с экрана.
                    if (panel->isOpen()) {
                        panel->close();
                        view->root().focus(FocusState::Programmatic);
                    } else if (reading && view->dismissOverlays()) {
                        // сноску закрыла полоса — больше ничего не нужно
                    } else if (isFullScreen()) {
                        setFullScreen(false);
                    } else if (reading) {
                        // Обратно туда, откуда книгу открыли: выбравший её на
                        // полке ждёт полку, а не заставку.
                        if (*bookCameFrom == Screen::Library) {
                            showLibrary();
                        } else {
                            showStartScreen();
                        }
                    } else if (*shown == Screen::Library) {
                        showStartScreen();
                    } else {
                        return;
                    }
                    break;
                default:
                    return;
            }
            args.handled(true);
        });
    };

    installKeys(screen->root());
    installKeys(shelf->root());
    installKeys(view->root());

    // ---- чем открыться ----
    //
    // Настройки одни и отвечают на один вопрос: показывать заставку или сразу
    // продолжать чтение. Реестр на этом пути не читается вовсе — путь к книге
    // лежит в настройках именно ради этого.
    bool const continueAtOnce = settings->continueReading &&
                                !settings->lastBookPath.empty() &&
                                std::filesystem::exists(settings->lastBookPath);

    if (continueAtOnce) {
        openBook(settings->lastBookPath);
    } else {
        window.content(screen->root());
    }

    window.activate();

    // Место и размер — после activate. До него окно ещё не создано настолько,
    // чтобы их принять: вызов отрабатывает, а показывается всё равно
    // минимальное.
    //
    // Размер по умолчанию ставится всегда, и лишь потом накрывается
    // запомненным. Иначе испорченная строка в настройках оставила бы окно
    // таким, каким его открыл WinUI: placement молча ничего не делает, когда
    // разбирать нечего, — и это правильно, но своё умолчание к тому моменту
    // должно быть уже на месте.
    // Перетаскивание книги в окно. После activate, потому что раньше окна ещё
    // нет, а регистрация цели идёт на его HWND.
    accept_file_drops(window, [openBook](std::vector<std::wstring> const& paths) {
        // Бросили пачку — открываем первую: читалка показывает одну книгу, а
        // добавлять остальные в реестр молча значило бы решать за читателя.
        if (!paths.empty()) openBook(paths.front());
    });

    appWindow.resize({kInitialWidth, kInitialHeight});
    if (!settings->windowPlacement.empty()) {
        window.placement(settings->windowPlacement);
    }

    // ---- сохранение места окна ----
    auto saveTimer = window.dispatcherQueue().createTimer();
    saveTimer.interval(kSaveQuiet);
    saveTimer.isRepeating(false);

    auto const rememberWindow = [window, settings] {
        settings->windowPlacement =
            std::wstring{reinterpret_cast<wchar_t const*>(window_placement(window).c_str())};
        saveSettings(*settings);
    };

    saveTimer.add_onTick([saveTimer, rememberWindow](Object const&, Object const&) {
        saveTimer.stop();
        rememberWindow();
    });

    // AppWindow.Changed дёргается и на перемещение, и на изменение размера, и
    // на смену presenter'а — то есть на всё, что мы запоминаем.
    appWindow.add_onChanged([saveTimer](Object const&, AppWindowChangedEventArgs&) {
        saveTimer.stop();   // каждое движение отодвигает запись
        saveTimer.start();
    });

    window.add_onClosed([saveTimer, positionTimer, rememberWindow, rememberPosition](Object const&,
                                                                              WindowEventArgs&) {
        saveTimer.stop();
        positionTimer.stop();
        rememberWindow();
        rememberPosition();
    });

    // Пауза заставки отсчитывается таймером очереди UI, а не сном: поток, на
    // котором стоит окно, обязан оставаться свободным.
    auto splashTimer = window.dispatcherQueue().createTimer();
    splashTimer.interval(kSplashHold);
    splashTimer.isRepeating(false);
    splashTimer.add_onTick([screen, splashTimer](Object const&, Object const&) {
        splashTimer.stop();
        screen->reveal();
    });
    if (!continueAtOnce) splashTimer.start();

    // Захват окна — это и есть то, что держит его живым, пока идёт
    // приложение; остальное держится за компанию. Пул STA не наш: его строит
    // и держит сама wxl в своей точке входа, и второй такой падает.
    return [window, screen, shelf, view, library, settings, saveTimer, positionTimer,
            splashTimer](Reason) {};
}
