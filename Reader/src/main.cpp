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
#include "imaging.h"
#include "library.h"
#include "settings.h"

#include "CompositionWindow.h"
#include "library_screen.h"
#include "skin_wizard.h"
#include "start_screen.h"

// Последним: он ведёт к модели книги, а она импортирует wxl.text, после чего
// стандартный заголовок MSVC уже не принимает.
#include "book.h"
#include "book_view.h"
#include "io.h"
#include "reader_panel.h"
#include "store.h"

// Импорт последним, после всех обычных заголовков.
import wxl.text;

using namespace wxl;
using namespace wxl::dsl;
using namespace std::chrono_literals;

namespace {

using namespace bukvitsa::reader;

using wxl::async::task;

// Пространство имён книги: `using namespace bukvitsa::reader` его не приносит,
// а обход каталога разбирает документ сам.
namespace fb3 = bukvitsa::fb3;

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
// возвращать. Мастер обложек — не экран, а оверлей поверх полосы: под ним
// читатель видит свою страницу, изогнутую редактируемыми кривыми.
enum class Screen { Start, Library, Book };

// ---- то, из чего собрано приложение ---------------------------------------
//
// Одна связка на всех, потому что корутины ниже держат её у себя в кадре, а
// девять отдельных параметров у каждой — это девять мест, где однажды забудут
// один. Всё внутри — либо shared_ptr, либо обёртка wxl, то есть ручка; копия
// такой связки ничего не копирует по существу.
struct App {
    Io* io = nullptr;
    std::shared_ptr<wxl::CompositionWindow> window;
    std::shared_ptr<Settings> settings;
    std::shared_ptr<Library> library;
    std::shared_ptr<BookState> state;
    std::shared_ptr<BookView> view;
    std::shared_ptr<ReaderPanel> panel;
    std::shared_ptr<LibraryScreen> shelf;
    std::shared_ptr<StartScreen> screen;
    std::shared_ptr<Screen> shown;
    std::shared_ptr<Screen> bookCameFrom;
    std::shared_ptr<Skins> skins;
};

// ---- корутины приложения --------------------------------------------------
//
// Каждая исполняется в интерфейсном потоке и уходит с него ровно на `co_await`
// — на время, пока рабочий поток читает или пишет файл. Между двумя co_await
// код обычный: он трогает XAML и общее состояние, потому что он и есть тот
// самый поток.

/// Пишет настройки. Копией, а не ссылкой: между co_await читатель успеет
/// поменять что-нибудь ещё, и на диск должно уйти то, что решили писать.
task saveSettingsLater(Io& io, Settings settings) {
    co_await io.writeFile(settingsPath(), settingsXml(settings));
}

/// Пишет состояние книги: место чтения и закладки.
task saveStateLater(Io& io, std::wstring guid, BookState state) {
    if (guid.empty()) co_return;

    co_await io.writeFile(statePath(guid), bookStateXml(state));
}

/// Пишет реестр.
task saveLibraryLater(Io& io, std::string xml) {
    co_await io.writeFile(libraryPath(), std::move(xml));
}

/// Достраивает полку: у каждой книги свой файл состояния, и читаются они по
/// одному, уже после того, как полка показана.
///
/// Это и есть «библиотека наполняется по мере чтения»: карточки встают сразу,
/// а «прочитано 42%» проступает на каждой, как только её файл прочитан. Полка
/// с сотней книг не ждёт сотни обращений к диску, чтобы показать первую.
task fillProgress(Io& io, std::shared_ptr<LibraryScreen> shelf, std::vector<BookEntry> books) {
    for (const BookEntry& book : books) {
        if (book.characterCount == 0) continue;   // не открывалась -- и читать нечего

        const std::optional<std::string> xml = co_await io.readFile(statePath(book.guid));

        if (!xml) continue;

        const BookState state = parseBookState(*xml);

        shelf->setProgress(book.guid, state.charOffset, state.bookmarks.size());
    }
}

/// Обходит каталог и добавляет из него книги — по одной, на глазах у читателя.
///
/// Здесь и видно, зачем всё это затевалось. Каталог перечисляется на рабочем
/// потоке; каждая книга читается там же; разбирается она здесь, между двумя
/// `co_await`, и сразу встаёт на полку. Ни один шаг не ждёт остальных: первая
/// книга появляется на полке, пока десятая ещё не прочитана, — а окно всё это
/// время отвечает, потому что интерфейсный поток каждый раз возвращается в
/// свой цикл сообщений.
///
/// Разбирается при этом `fb3::Document`, а не `Book`: реестру нужны метаданные
/// и обложка, а движок вёрстки с пагинатором книге, которую никто не открывал,
/// ни к чему.
task addFolderFlow(App app, std::filesystem::path folder, std::function<void()> showLibrary) {
    Io& io = *app.io;

    // Полка -- прежде обхода: читатель, добавивший каталог, должен видеть, как
    // тот наполняется, а не пустой стартовый экран, за которым что-то
    // происходит.
    showLibrary();

    const std::vector<DirectoryEntry> found = co_await io.list(folder, L"*.fb3");

    bool added = false;

    for (const DirectoryEntry& entry : found) {
        if (entry.isDirectory) continue;

        const std::filesystem::path path = folder / entry.name;

        const std::optional<std::string> bytes = co_await io.readFile(path);

        if (!bytes) continue;

        const std::uint64_t fileSize = bytes->size();

        // Разбор -- единственное место здесь, которое может бросить, и ловится
        // он вокруг разбора, а не вокруг всего шага: не книга, битая книга,
        // книга от будущего формата — не повод бросать обход. Каталог с сотней
        // файлов не должен спотыкаться об один.
        std::optional<fb3::Document> document;

        try {
            document.emplace(std::move(*bytes));
        } catch (const std::exception&) {
            continue;
        }

        const std::size_t knownBefore = app.library->books().size();

        const BookEntry stored = app.library->add(*document, path, fileSize);

        const bool isNew = app.library->books().size() != knownBefore;

        if (const CoverBytes cover = coverOf(*document, stored.guid); !cover.name.empty())
            co_await io.writeFile(coverDirectory() / cover.name, std::string(cover.bytes));

        added = true;

        // Полка растёт на каждой книге, а не в конце: в этом и смысл — читатель
        // видит, как она наполняется. Одной карточкой, а не пересборкой всей
        // полки: та стоила бы квадрата от числа книг и стирала бы прогресс,
        // который к тому времени уже проступил на соседях.
        if (isNew && *app.shown == Screen::Library) app.shelf->appendBook(stored);
    }

    if (added) co_await io.writeFile(libraryPath(), app.library->toXml());
}

/// Сохраняет обложку из мастера: копия снимка, запись реестра, немедленное
/// применение — сохранённая обложка тут же становится текущей темой.
task saveSkinFlow(App app, Skin skin, std::filesystem::path photo,
                  std::function<void()> leaveWizard) {
    Io& io = *app.io;

    // У правки старой обложки копия снимка уже лежит в skins\ — скопировать
    // надо только новый. Имя копии — новый guid с родным расширением: имена
    // обложек выбирает читатель и они могут повторить друг друга, а guid —
    // нет.
    if (skin.image.empty()) {
        const std::optional<std::string> bytes = co_await io.readFile(photo);

        if (!bytes) {
            ::MessageBoxW(app.window->handle(),
                          (L"Не удалось прочитать снимок:\n" + photo.wstring()).c_str(),
                          L"Буквица", MB_OK | MB_ICONWARNING);
            co_return;
        }

        std::wstring file = newGuid() + photo.extension().wstring();

        co_await io.writeFile(skinDirectory() / file, std::move(*bytes));

        skin.image = std::move(file);
    }

    const std::wstring skinName = skin.name;
    app.skins->put(std::move(skin));

    co_await io.writeFile(skinsPath(), app.skins->toXml());

    app.view->setSkins(app.skins->list());
    app.panel->refreshThemes();

    const std::vector<Skin>& list = app.skins->list();
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (list[index].name == skinName) {
            app.view->setTheme(kThemeCount + static_cast<int>(index));
            break;
        }
    }

    app.settings->skin = skinName;
    co_await io.writeFile(settingsPath(), settingsXml(*app.settings));

    leaveWizard();
}

/// «Продолжить чтение»: найти книгу, которую читали, и открыть её.
///
/// Путь в настройках — копия того, что в реестре, и она там ради быстрого
/// пути. Протух — спрашиваем реестр по guid; нет и там — читателю нечего
/// продолжать, и он хотел открыть книгу.
task continueReading(Io& io, std::shared_ptr<Settings> settings, std::shared_ptr<Library> library,
                     std::function<void(std::filesystem::path)> openBook,
                     std::function<void()> addBook) {
    std::filesystem::path path = settings->lastBookPath;

    if (!path.empty() && !co_await io.fileExists(path)) path.clear();

    if (path.empty()) {
        if (const BookEntry* entry = library->find(settings->lastBookGuid)) path = entry->path;

        if (!path.empty() && !co_await io.fileExists(path)) path.clear();
    }

    if (path.empty()) {
        addBook();
        co_return;
    }

    openBook(path);
}

/// Открывает книгу: от байтов на диске до страницы на экране.
///
/// Порядок здесь -- это порядок обязательств. Сначала книга разбирается (и
/// только если разобралась, старая уступает ей место), потом на диск уходит
/// место чтения предыдущей, и лишь затем реестр, обложка, настройки и
/// состояние новой. Каждый `co_await` -- это выход в цикл сообщений: окно всё
/// это время живо, отвечает и перерисовывается.
task openBookFlow(App app, std::filesystem::path path) {
    Io& io = *app.io;

    const std::optional<std::string> bytes = co_await io.readFile(path);

    if (!bytes) {
        ::MessageBoxW(app.window->handle(),
                      (L"Не удалось прочитать файл книги:\n" + path.wstring()).c_str(), L"Буквица",
                      MB_OK | MB_ICONWARNING);
        co_return;
    }

    const std::uint64_t fileSize = bytes->size();

    std::shared_ptr<Book> book;

    try {
        book = std::make_shared<Book>(path, std::move(*bytes), dwriteFactory());
    } catch (std::exception const& failure) {
        // Разговор с читателем, а не запись в лог: он только что выбрал этот
        // файл и вправе узнать, что с ним не так.
        wxl::text::u16_text const reason = wxl::text::assume_valid(failure.what()).to_utf16();
        std::wstring const complaint = L"Не удалось открыть книгу:\n" + path.wstring() + L"\n\n" +
                                       std::wstring(reason.wchars());
        ::MessageBoxW(app.window->handle(), complaint.c_str(), L"Буквица",
                      MB_OK | MB_ICONWARNING);
        co_return;
    }

    // Место чтения предыдущей книги — на диск сразу: сейчас settings укажет на
    // другую, и записывать станет некуда.
    if (!app.settings->lastBookGuid.empty() && app.view->isOpen()) {
        app.state->charOffset = app.view->readingPosition();

        co_await io.writeFile(statePath(app.settings->lastBookGuid), bookStateXml(*app.state));
    }

    // Копией, а не ссылкой: между co_await реестр может дополниться, и вектор
    // переедет вместе со всеми ссылками в него.
    const BookEntry stored = app.library->add(book->document(), path, fileSize);

    if (const CoverBytes cover = coverOf(book->document(), stored.guid); !cover.name.empty())
        co_await io.writeFile(coverDirectory() / cover.name, std::string(cover.bytes));

    co_await io.writeFile(libraryPath(), app.library->toXml());

    app.settings->lastBookGuid = stored.guid;
    app.settings->lastBookPath = stored.path;

    co_await io.writeFile(settingsPath(), settingsXml(*app.settings));

    const std::optional<std::string> stateXml = co_await io.readFile(statePath(stored.guid));

    *app.state = stateXml ? parseBookState(*stateXml) : BookState{};

    app.view->open(std::move(book), app.state->charOffset);
    app.panel->setState(app.state.get());

    // Сверстать и нарисовать до показа. Полоса займёт то же место, что и экран,
    // который сейчас на нём стоит, — а у него и спрашиваем размер с масштабом.
    // Иначе читатель, нажав «Продолжить чтение», успевает увидеть пустой лист:
    // элемент попадает в дерево сразу, а рисовать его есть чем только со
    // следующего кадра.
    if (Nullable<UIElement> const showing = app.window->content()) {
        if (Nullable<XamlRoot> const root = showing->xamlRoot()) {
            Size const area = root->size();
            app.view->prepare(area.width, area.height,
                              static_cast<float>(root->rasterizationScale()));
        }
    }

    *app.bookCameFrom = *app.shown;
    *app.shown = Screen::Book;
    // Полоса становится текущим экраном: показать её страницу на сцене и увести
    // задник окна с заставки на бумагу темы. Только потом — остров ввода поверх.
    app.view->setActive(true);
    app.window->content(app.view->root());
}

/// Запуск: настройки, реестр, первый экран и только потом -- показ окна.
///
/// Окно строится пустым и невидимым, а показывается в конце: место, куда его
/// поставить, лежит в настройках, и открыть его сначала посреди экрана, а
/// потом переставить -- значит показать читателю прыжок. Ждать при этом нечего:
/// файл настроек читает рабочий поток, а этот тем временем уже крутит цикл
/// сообщений.
task startupFlow(App app, wxl::DispatcherQueueTimer splashTimer,
                 std::function<void(std::filesystem::path)> openBook,
                 std::function<void()> showStartScreen) {
    Io& io = *app.io;

    const std::optional<std::string> settingsXmlText = co_await io.readFile(settingsPath());

    *app.settings = parseSettings(settingsXmlText.value_or(std::string{}));

    app.view->setFontSize(app.settings->fontSize);
    app.view->setLineHeight(app.settings->lineHeight);
    app.view->setMargin(app.settings->margin);

    // Обложки — раньше темы: выбранной темой может оказаться обложка, а её
    // индекс продолжает список за встроенными и без реестра не существует.
    const std::optional<std::string> skinsXmlText = co_await io.readFile(skinsPath());

    app.skins->loadFrom(skinsXmlText.value_or(std::string{}));
    app.view->setSkins(app.skins->list());
    app.panel->refreshThemes();

    int theme = app.settings->theme;

    if (!app.settings->skin.empty()) {
        const std::vector<Skin>& list = app.skins->list();
        for (std::size_t index = 0; index < list.size(); ++index) {
            if (list[index].name == app.settings->skin) {
                theme = kThemeCount + static_cast<int>(index);
                break;
            }
        }
    }
    app.view->setTheme(theme);

    // Реестр читается всегда, а не только когда показывают полку: он маленький,
    // читает его чужой поток, и без него не ответить на «продолжить чтение» по
    // guid, если путь в настройках протух.
    const std::optional<std::string> libraryXmlText = co_await io.readFile(libraryPath());

    app.library->loadFrom(libraryXmlText.value_or(std::string{}));

    // Продолжать чтение — только если книга на месте. Путь в настройках копия
    // того, что в реестре, и она здесь ради быстрого пути; протухла —
    // спрашиваем реестр по guid.
    std::filesystem::path lastBook = app.settings->lastBookPath;

    if (!lastBook.empty() && !co_await io.fileExists(lastBook)) lastBook.clear();

    if (lastBook.empty()) {
        if (const BookEntry* entry = app.library->find(app.settings->lastBookGuid)) {
            lastBook = entry->path;

            if (!co_await io.fileExists(lastBook)) lastBook.clear();
        }
    }

    app.window->resize({kInitialWidth, kInitialHeight});

    // Размер по умолчанию ставится всегда, и лишь потом накрывается
    // запомненным. Иначе испорченная строка в настройках оставила бы окно
    // таким, каким его открыл WinUI: placement молча ничего не делает, когда
    // разбирать нечего, — и это правильно, но своё умолчание к тому моменту
    // должно быть уже на месте.
    if (!app.settings->windowPlacement.empty()) app.window->placement(app.settings->windowPlacement);

    if (app.settings->continueReading && !lastBook.empty()) {
        openBook(lastBook);
    } else {
        showStartScreen();
        splashTimer.start();
    }

    app.window->activate();
}

}  // namespace

wxl::Teardown wxl_launched() {
    // Пустые: их наполнит запуск, и наполнит асинхронно. Ни настройки, ни
    // реестр здесь не читаются — в этом потоке к диску не обращаются вовсе.
    auto settings = std::make_shared<Settings>();
    auto library = std::make_shared<Library>();

    // Своё окно верхнего уровня на композиторе, а не генерируемое wxl::Window:
    // у того верхнее окно перенаправляемое, и при быстрой растяжке за угол в
    // просвете белеет его GDI-поверхность, стёртая системной кистью. Здесь окно
    // с WS_EX_NOREDIRECTIONBITMAP — поверхности перенаправления нет вовсе, а
    // содержимое целиком даёт композитор. Move-only (владеет HWND и островами),
    // поэтому в shared_ptr: связка App и Teardown держат его копией.
    auto window = std::make_shared<CompositionWindow>(L"Буквица", SizeInt32{720, 520});

    // Заставка — задником сцены: единственный визуал под XAML-островом
    // оснастки. Он ресайзится синхронно в WM_SIZE, оттого держится за рамку без
    // отставания, и в просвете при быстрой растяжке видна заставка, а не белое.
    // Первую картинку грузим синхронно — окно до того скрыто; смены фона на ходу
    // пойдут через backgroundAsync. Разбор — в solutions.md wxl, «Своё окно на
    // композиторе».
    // Заставка — задником сцены (визуал под XAML-островом оснастки): он
    // ресайзится синхронно в WM_SIZE, и просвет при растяжке продолжает
    // заставку, а не белеет. Грузим асинхронно через Win2D/TextureCache, а не
    // синхронным background(path): тот рисует свою DrawingSurface отдельным
    // D3D-устройством, и на сцене-композиторе оно не уживается с устройством
    // Win2D того же кэша — DirectComposition падает (dcompi). Асинхронная
    // загрузка идёт тем же устройством Win2D и не падает. TODO фазы B: разобраться
    // с композицией задника (окно пока выглядит прозрачным — задник не виден).
    window->backgroundAsync(exeDirectory() / L"Assets/splash-screen-1k.png");

    // Ввод-вывод поднимается сразу за окном: раньше нельзя (нужна его очередь),
    // позже незачем (первое, что делает приложение, — читает настройки).
    auto io = std::make_shared<Io>();
    io->start(window->dispatcherQueue());

    auto screen = std::make_shared<StartScreen>(window->chromeCompositor());
    auto view = std::make_shared<BookView>(*window);
    auto shelf = std::make_shared<LibraryScreen>();
    auto skins = std::make_shared<Skins>();
    auto wizard = std::make_shared<SkinWizard>(window->chromeCompositor());

    // Панель живёт поверх полосы набора: «поверх страницы» — это внутри полосы,
    // а не рядом с ней.
    auto panel = std::make_shared<ReaderPanel>(window->chromeCompositor(), *view);
    view->addOverlay(panel->root());

    // Состояние открытой книги: место чтения и закладки. Читается и пишется
    // целиком, потому что файл переписывается заменой — «дописать одно поле»
    // всё равно значит написать его весь.
    auto state = std::make_shared<BookState>();

    // Настройки чтения общие для всех книг: читателю нужен один привычный вид,
    // а не разный шрифт в каждой книге. Ставит их запуск, когда прочитает файл.

    // ---- сохранение места чтения ----
    //
    // Отложенно, как и место окна: перелистывание — самое частое действие в
    // читалке, а файл состояния книги переписывается целиком.
    auto positionTimer = window->dispatcherQueue().createTimer();
    positionTimer.interval(kPositionQuiet);
    positionTimer.isRepeating(false);

    auto const rememberPosition = [io, view, settings, state] {
        // Книга закрыта -- писать нечего: место чтения принадлежит ей, а не
        // окну, и ноль незанятой полосы стёр бы то, что уже записано.
        if (settings->lastBookGuid.empty() || !view->isOpen()) return;
        state->charOffset = view->readingPosition();
        io->spawn(saveStateLater(*io, settings->lastBookGuid, *state));
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

    auto const showStartScreen = [window, screen, view, settings, library, shown, rememberPosition,
                                  closePanel] {
        closePanel();
        // Уходя из книги, место чтения пишем сразу: отложенная запись ждёт
        // паузы, а читатель уже ушёл -- и, может быть, закроет приложение
        // раньше, чем таймер сработает.
        rememberPosition();

        // Полоса перестаёт быть текущим экраном: убрать её страницу со сцены и
        // вернуть задником окна заставку — под стартовым экраном она, а не
        // бумага книги. Задел на будущее (кэш texture держит снимок) — второй
        // показ заставки идёт без загрузки.
        view->setActive(false);
        window->backgroundAsync(exeDirectory() / L"Assets/splash-screen-1k.png");

        // Большой кнопке — её книга: обложка, название, автор. На каждом
        // показе, потому что последняя открытая книга могла смениться, пока
        // экрана не было видно; при запуске реестр к этому моменту прочитан.
        if (const BookEntry* entry = library->find(settings->lastBookGuid)) {
            screen->setContinueBook(entry->title, entry->authors,
                                    entry->cover.empty() ? std::filesystem::path{}
                                                         : coverDirectory() / entry->cover);
        }

        *shown = Screen::Start;
        window->content(screen->root());
    };

    // Всё, из чего собрано приложение, одной связкой: её берут корутины.
    App const app{io.get(), window, settings, library,      state, view,
                  panel,    shelf,  screen,   shown,        bookCameFrom, skins};

    auto const showLibrary = [io, app, window, shelf, library, settings, shown, rememberPosition,
                              closePanel] {
        closePanel();
        rememberPosition();   // и полка тут же покажет свежий процент

        // Как и на стартовом экране: полоса перестаёт быть текущим экраном —
        // её страница уходит со сцены, а задником окна снова заставка.
        app.view->setActive(false);
        window->backgroundAsync(exeDirectory() / L"Assets/splash-screen-1k.png");

        // Полка пересобирается на каждый показ: книга могла добавиться, а
        // место чтения — уехать с тех пор, как её видели в прошлый раз.
        shelf->show(*library, settings->continueReading);
        *shown = Screen::Library;
        window->content(shelf->root());

        // Карточки уже стоят; проценты проступят на них по мере того, как
        // рабочий поток прочитает файлы состояния — по одному на книгу.
        io->spawn(fillProgress(*io, shelf, library->books()));
    };

    auto const openBook = [io, app](std::filesystem::path const& path) {
        io->spawn(openBookFlow(app, path));
    };

    auto const addBook = [window, openBook] {
        std::filesystem::path const path = askForBook(window->handle());
        if (!path.empty()) openBook(path);
    };

    screen->onAddBook = addBook;
    auto const addFolder = [io, app, window, showLibrary] {
        std::filesystem::path const folder = askForFolder(window->handle());

        if (!folder.empty()) io->spawn(addFolderFlow(app, folder, showLibrary));
    };

    screen->onAddFolder = addFolder;
    screen->onLibrary = showLibrary;

    // Отмена стартового экрана — выход из приложения: обычное закрытие окна,
    // со всем, что оно сохраняет по дороге.
    screen->onExit = [window] { window->close(); };

    shelf->onAddBook = addBook;
    shelf->onBack = showStartScreen;
    shelf->onOpen = [library, openBook](std::wstring guid) {
        if (BookEntry const* entry = library->find(guid)) openBook(entry->path);
    };
    shelf->onContinueAtStartChanged = [io, settings](bool wanted) {
        settings->continueReading = wanted;
        io->spawn(saveSettingsLater(*io, *settings));
    };

    panel->onStateChanged = [io, settings, state] {
        io->spawn(saveStateLater(*io, settings->lastBookGuid, *state));
    };

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

    panel->onSettingsChanged = [io, settings, view] {
        // Обложка запоминается именем, встроенная тема — номером; прежний
        // номер при обложке остаётся как то, куда вернуться, если реестр
        // обложек пропадёт.
        if (const Skin* active = view->activeSkin()) {
            settings->skin = active->name;
        } else {
            settings->skin.clear();
            settings->theme = view->theme();
        }
        settings->fontSize = view->fontSize();
        settings->lineHeight = view->lineHeight();
        settings->margin = view->margin();
        io->spawn(saveSettingsLater(*io, *settings));
    };

    // ---- мастер обложек ----
    //
    // Оверлей поверх полосы: под сеткой мастера читатель видит свою страницу,
    // изогнутую редактируемыми кривыми. Дорога туда — кнопки в панели «Вид»,
    // дорога обратно — его собственные кнопки; Escape мастером не занимается.
    view->addOverlay(wizard->root());

    // Предпросмотр: полоса рисуется со снимком и кривыми мастера. Он же —
    // пересчёт после отпускания точки.
    auto const previewSkin = [view, wizard] {
        view->setPreview(&wizard->skin(), wizard->imagePath());
    };

    auto const leaveWizard = [view, wizard] {
        wizard->hide();
        view->setPreview(nullptr, {});
        view->root().focus(FocusState::Programmatic);
    };

    auto const badImage = [window](std::filesystem::path const& path) {
        ::MessageBoxW(window->handle(),
                      (L"Не удалось открыть изображение:\n" + path.wstring()).c_str(), L"Буквица",
                      MB_OK | MB_ICONWARNING);
    };

    panel->onAddSkin = [window, wizard, closePanel, badImage, previewSkin] {
        std::filesystem::path const path = askForImage(window->handle());

        if (path.empty()) return;

        if (!wizard->openNew(path)) {
            badImage(path);
            return;
        }

        closePanel();
        wizard->show();
        previewSkin();
    };

    panel->onEditSkin = [window, wizard, skins, closePanel, badImage,
                         previewSkin](std::wstring skinName) {
        const Skin* known = skins->find(skinName);
        if (!known) return;   // реестр успел перемениться под руками

        if (!wizard->openEdit(*known)) {
            badImage(skinDirectory() / known->image);
            return;
        }

        closePanel();
        wizard->show();
        previewSkin();
    };

    wizard->onCurvesChanged = previewSkin;

    wizard->onChooseAnother = [window, wizard, badImage, previewSkin] {
        std::filesystem::path const path = askForImage(window->handle());

        // Отказался — остаёмся на прежнем снимке: читатель ничего не терял.
        if (path.empty()) return;

        if (!wizard->openNew(path)) {
            badImage(path);
            return;
        }
        previewSkin();
    };

    wizard->onExit = leaveWizard;

    wizard->onSave = [io, app, leaveWizard](Skin skin, std::filesystem::path photo) {
        io->spawn(saveSkinFlow(app, std::move(skin), std::move(photo), leaveWizard));
    };

    screen->onContinueReading = [io, settings, library, openBook, addBook] {
        io->spawn(continueReading(*io, settings, library, openBook, addBook));
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
    // Флага «мы в полном экране» нет намеренно: он был бы вторым местом, где это
    // записано, — а состояние знает само окно (window->fullScreen()), и второй
    // его слепок разошёлся бы с первым.
    auto const isFullScreen = [window] { return window->fullScreen(); };

    auto const setFullScreen = [window](bool on) { window->fullScreen(on); };

    auto const installKeys = [setFullScreen, isFullScreen, shown, bookCameFrom, showStartScreen,
                              showLibrary, panel, view, wizard](UIElement const& element) {
        element.add_onPreviewKeyDown([=](Object const&, KeyRoutedEventArgs& args) {
            if (args.handled()) return;   // полоса набора своё уже разобрала

            // Пока открыт мастер обложек, клавиши экранов молчат: Escape увёл
            // бы с полосы прямо под ним. Дороги из мастера — его кнопки.
            if (wizard->isOpen()) return;

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

    // Перетаскивание книги в окно. Регистрация цели идёт на HWND, который к
    // этому моменту уже есть -- окно создано, хотя ещё и не показано. Пачку из
    // нескольких файлов окно уже свело к первому пути: читалка показывает одну
    // книгу, а добавлять остальные в реестр молча значило бы решать за читателя.
    window->acceptFileDrops([openBook](std::filesystem::path path) { openBook(path); });

    // ---- сохранение места окна ----
    auto saveTimer = window->dispatcherQueue().createTimer();
    saveTimer.interval(kSaveQuiet);
    saveTimer.isRepeating(false);

    auto const rememberWindow = [io, window, settings] {
        settings->windowPlacement = window->placement();
        io->spawn(saveSettingsLater(*io, *settings));
    };

    saveTimer.add_onTick([saveTimer, rememberWindow](Object const&, Object const&) {
        saveTimer.stop();
        rememberWindow();
    });

    // Геометрия сменилась — двигали, растягивали, максимизировали или
    // восстановили: всё, что мы запоминаем. Окно сводит это в один колбэк.
    window->onGeometryChanged([saveTimer] {
        saveTimer.stop();   // каждое движение отодвигает запись
        saveTimer.start();
    });

    window->onClosed([saveTimer, positionTimer, rememberWindow, rememberPosition] {
        saveTimer.stop();
        positionTimer.stop();
        rememberWindow();
        rememberPosition();
    });

    // Пауза заставки отсчитывается таймером очереди UI, а не сном: поток, на
    // котором стоит окно, обязан оставаться свободным.
    auto splashTimer = window->dispatcherQueue().createTimer();
    splashTimer.interval(kSplashHold);
    splashTimer.isRepeating(false);
    splashTimer.add_onTick([screen, splashTimer](Object const&, Object const&) {
        splashTimer.stop();
        screen->reveal();
    });

    // ---- запуск ----
    //
    // Всё, что читалка знает о себе, читается отсюда и асинхронно: настройки,
    // реестр, книга, на которой остановились. Окно показывается в конце этой
    // цепочки — уже на своём месте и с уже выбранным экраном.
    io->spawn(startupFlow(app, splashTimer, openBook, showStartScreen));

    // Захват окна — это и есть то, что держит его живым, пока идёт
    // приложение; остальное держится за компанию. Пул STA не наш: его строит
    // и держит сама wxl в своей точке входа, и второй такой падает.
    //
    // Рабочий поток останавливается здесь же: очередь интерфейсного к этому
    // моменту уже не принимает заданий, и операции, не успевшие вернуться,
    // возобновлять некому и незачем.
    return [io, window, screen, shelf, view, library, settings, skins, wizard, saveTimer,
            positionTimer, splashTimer](Reason) { io->stop(); };
}
