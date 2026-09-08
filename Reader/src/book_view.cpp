#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>
#include <vector>

// dwrite.h первым: он приводит guiddef.h с DEFINE_GUID, без которого
// d2d1effects.h не раскрывает CLSID эффектов.
#include <dwrite.h>

#include <d2d1effects.h>

// Родной размер снимка подложки в пикселях — до создания поверхности,
// поэтому нужен настоящий интерфейс WIC, а не только его объявление.
#include <wincodec.h>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. CompositionWindow
// первым из них: у него свои стандартные заголовки, а после импорта их не
// подключить; сам он к импорту не ведёт.
#include "CompositionWindow.h"

// Свой следующим: он единственный тянет за собой стандартные заголовки,
// которых нет здесь, — и уже он ведёт к импорту модуля книги.
#include "book_view.h"

#include "imaging.h"

#include "bukvitsa/typography/block.h"
#include "bukvitsa/typography/glyph_painter.h"

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

namespace {

/// Мера полосы — длина строки в знаках, а не доля окна: слишком длинная строка
/// перестаёт читаться, глаз теряет начало следующей. Сколько знаков в строке —
/// известно точно, потому что известны шрифт и кегль; правило «шире трёх пятых
/// окна» кегля не знает и ошибается там, где важнее всего: на кегле 28 широкое
/// окно прекрасно читается в одну колонку, на кегле 14 то же окно — уже нет.
///
/// Восемьдесят пять, а не классические семьдесят: колонка на экране не то же
/// самое, что колонка в книге. Экран шире разворота, строка на нём длиннее
/// естественным образом, и разбивать её на колонки раньше времени — значит
/// дробить страницу там, где читателю удобнее целая.
constexpr float kMaxLineChars = 85.0f;

/// Нижняя граница новой колонки. Уже шестидесяти знаков выключенная строка без
/// переносов начинает рваться дырами между словами — переносов у нас пока нет,
/// и узкая колонка обошлась бы дороже широкой.
constexpr float kMinLineChars = 60.0f;

/// Гистерезис в знаках. Один знак и только для окна, которое тянут мышью:
/// см. обработчик sizeChanged.
constexpr float kColumnHysteresis = 1.0f;

/// Средник — расстояние между колонками, в долях поля. Равен полю: просветы
/// у краёв окна и посередине разворота — одной ширины. Слиться колонкам это
/// не даёт, потому что по среднику лежит тень корешка — граница у них есть,
/// и шире зазора для неё не нужно.
constexpr float kGutterOfMargin = 1.0f;

/// Отступы сверху и снизу, в DIP. Регулировка «Поля» правит только
/// горизонтальные поля: ими читатель выбирает ширину строки, а высоте полосы
/// выбирать нечего — она и так вся, что осталось от окна.
constexpr float kVerticalMargin = 50.0f;

/// Прогиб страницы на фотографии-подложке, в DIP: насколько верхняя строка в
/// середине страницы поднимается, а нижняя опускается. Модель — вертикальное
/// «брюхо» выпуклой бумаги: смещение равно произведению купола по X (ноль у
/// корешка и наружных краёв, максимум в середине каждой страницы) на глубину
/// по Y (ноль в середине полосы, максимум у верха и низа). У корешка и краёв
/// строки прямые — там бумага снимка прижата.
constexpr float kBulge = 8.0f;

/// Размытие набора под конфигуратором изгиба, в DIP. Лёгкое, не туман:
/// тексту достаточно отступить на второй план, чтобы направляющие мастера
/// читались чётче, — но кривые укладывают по строкам, и строки должны
/// оставаться различимыми.
constexpr float kPreviewBlur = 1.5f;

/// На чём меряется средняя ширина знака. Не алфавит: в строке книги есть
/// пробелы и запятые, и они тоже знаки. Обе фразы — панграммы, то есть в
/// каждой все буквы своего алфавита ровно по разу.
constexpr wchar_t kCyrillicSample[] =
    L"съешь же ещё этих мягких французских булок, да выпей чаю";
constexpr wchar_t kLatinSample[] = L"the quick brown fox jumps over the lazy dog";

/// Пауза, после которой отложенная перевёрстка случается. Короче автоповтора
/// клавиши: пока плюс держат, перевёрстка так и не начинается.
// Сколько времени пагинатору отдаётся за раз. Порция кончается не раньше
// срока, а позже — на цену одного блока: разорвать вёрстку абзаца нечем.
// Самый дорогой блок из четырёх тестовых книг стоит 27 мс, так что худшая
// порция укладывается в те сто миллисекунд, дольше которых читалка не вправе
// не отвечать на ввод. Роман в 650 тысяч знаков проходит за шесть порций.
constexpr auto kPaginationSlice = 50ms;

/// Переворот листа. Полсекунды: движение руки укладывается и в половину этого,
/// но лист — не курсор, а страница книги, и глазу нужно успеть увидеть, что
/// именно перевернулось. Дольше читатель начнёт ждать.
constexpr auto kTurn = 480ms;

/// На сколько лист поворачивается, уходя. Плоский лист, повёрнутый на десять
/// градусов вокруг своего левого края, читается как поднятая бумага — большего
/// объёма тут и не нужно, весь остальной делает тень.
constexpr float kTurnAngle = 10.0f;

/// Его же косинус и синус — числами, а не вызовом. Угол известен на этапе
/// компиляции, а `<cmath>` сюда уже не включить: заголовок, включённый после
/// import std;, MSVC не принимает.
constexpr float kTurnCos = 0.98481f;
constexpr float kTurnSin = 0.17365f;

/// Тень уезжающего листа: размытие и сдвиг вправо. Она не украшение — лист и
/// страница под ним одного цвета, и без тени глаз не видит, что один поднят
/// над другим.
constexpr float kShadowBlur = 24.0f;
constexpr float kShadowShift = 6.0f;
constexpr float kShadowOpacity = 0.45f;

/// Насколько тень выходит за лист — и, стало быть, насколько крой листа
/// отпущен в покое. Клип режет не только содержимое визуала, но и то, что
/// визуал отбрасывает, а тень уезжающего листа лежит как раз за его правым
/// краем: нулевые отступы срезали бы её начисто (проверено — от неё остаётся
/// линия в один пиксель). Отрицательный отступ клип расширяет.
constexpr float kShadowReach = kShadowShift + kShadowBlur;

/// Книжное листание: сколько едет кромка от края разворота до корешка. Пути
/// вдвое меньше, чем у обычного переворота, и рука тянет бумагу ровно, без
/// разгона, — отсюда и время меньше kTurn, и выключка линейная.
///
/// ВРЕМЕННО замедлено вдесятеро против рабочих 280 мс: так переворот видно
/// глазом и по кадрам. Вернуть перед тем, как читать книгу.
constexpr auto kLeafSlide = 500ms;

/// Насколько широко тень расходится к концу переворота — в долях страницы, а
/// не в точках. Тень меряется окном: на широком экране полсотни точек
/// теряются, на узком закрывают текст. Целая страница в конце означает ровно
/// половину страницы на середине переворота, а дальше только шире.
constexpr float kFoldOfPage = 1.0f;

/// Плотность тени в начале и в конце. Расходясь, полутень светлеет: иначе
/// впятеро более широкая полоса читалась бы не мягче, а тяжелее — а мягкость
/// тут и есть признак того, что бумага поднялась.
constexpr float kFoldDense = 1.0f;
constexpr float kFoldFaint = 0.5f;

/// Плотность у самой кромки и в середине спада. Тут и у остальных теней
/// записана только прозрачность: тон у всех общий и берётся из темы, потому
/// что тень на бумаге уходит в её же тон, а не в серое.
constexpr std::uint32_t kFoldNear = 0x96000000;
constexpr std::uint32_t kFoldMid = 0x36000000;

/// Куда по ходу переворота уезжает середина спада. В начале он сосредоточен у
/// кромки — тень читается краем; к концу выравнивается и края у тени не
/// остаётся вовсе. Это и есть «мягче», всё остальное делает ширина.
constexpr float kFoldMidStop = 0.30f;
constexpr float kFoldMidSoft = 0.62f;

/// Тень, которую приходящий лист кладёт наружным краем на страницу под ним.
/// Она узкая и по ходу переворота не меняется: этот край всё время лежит на
/// бумаге, в отличие от корешкового, который поднят и оттого мажет широко.
/// Без неё лист не читается отдельной бумагой поверх страницы — а на неё вся
/// вторая половина переворота и опирается. Тоже в долях окна.
constexpr float kEdgeOfWindow = 0.026f;
constexpr std::uint32_t kEdgeNear = 0x70000000;
constexpr std::uint32_t kEdgeMid = 0x26000000;
constexpr float kEdgeMidStop = 0.42f;

/// Притенение самого листа у сгиба — то, чем плоская бумага получает
/// объём: у корешка лист отходит от страницы и уходит в тень собственного
/// изгиба. Это не тень на чём-то другом, а полутон на самой бумаге, потому
/// полоска и лежит ребёнком листа: его же клип не даёт ей вылезти на страницу
/// под ним, пока лист узкий. Плотность — ровно та, что у тени поднятой
/// бумаги, и это не совпадение, а требование: средник разворота обязан быть
/// того же цвета, что тень при листании, а полутон обязан совпадать со
/// средником (см. alphaOf) — значит, все трое носят одну плотность.
constexpr float kBendOfWindow = 0.045f;
constexpr std::uint32_t kBendNear = kFoldNear;
constexpr std::uint32_t kBendMid = kFoldMid;
constexpr float kBendMidStop = 0.45f;

/// Когда движущиеся тени передают средник нарисованному. Не в самом конце:
/// подмена должна успеть пройти незаметно, пока лист доходит последние
/// проценты пути.
constexpr float kHandover = 0.78f;

/// Прозрачность из записанного цвета. Нужна затем, чтобы средник и полутон
/// изгиба брали её из одного места: они обязаны совпасть, иначе подмена в
/// конце переворота будет видна ступенькой.
constexpr float alphaOf(std::uint32_t argb) {
    return static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
}

/// Цвет тени темы с нужной прозрачностью — в том виде, в каком его берёт
/// композитор. Прозрачный конец градиента тоже красится тоном, а не сводится
/// к прозрачному чёрному: интерполируй композитор без предумножения, чистая
/// прозрачность увела бы спад в серое.
Color tinted(const D2D1_COLOR_F& color, float alpha) {
    auto channel = [](float value) {
        return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return ARGB{channel(alpha), channel(color.r), channel(color.g), channel(color.b)};
}

/// То же для Direct2D, которым рисуется средник.
D2D1_COLOR_F tintedF(const D2D1_COLOR_F& color, float alpha) {
    return {color.r, color.g, color.b, alpha};
}

/// Цвет темы в том виде, в каком его берёт XAML. Нужен ровно однажды — для
/// подложки под страницей, — поэтому тут, а не в theme.h: там цвета лежат
/// такими, какими их берёт Direct2D, и это их главное место работы.
constexpr std::uint32_t argbOf(const D2D1_COLOR_F& color) {
    auto channel = [](float value) { return static_cast<std::uint32_t>(value * 255.0f + 0.5f); };
    return 0xFF000000u | (channel(color.r) << 16) | (channel(color.g) << 8) | channel(color.b);
}

bool controlHeld() {
    // Клавиатурные модификаторы у KeyRoutedEventArgs не спросить: WinUI их там
    // не отдаёт. Состояние клавиши знает Win32, и вопрос к нему — один вызов.
    return (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

}  // namespace

BookView::BookView(CompositionWindow& window)
    : window_(&window),
      compositor_(window.compositor()),
      queue_(window.dispatcherQueue()),
      // Всплывашка сноски — XAML-остров, её рисунок висит в дереве острова, а не
      // на сцене: ей нужен композитор острова, а не окна.
      note_(window.chromeCompositor()) {
    root_ = buildTree();

    IDWriteFactory* const dwrite = dwriteFactory();
    if (!dwrite) return;

    // Колонцифра — единственный текст в читалке, который рисуется не своей
    // вёрсткой: у него нет ни переносов, ни выключки, и ради одной строки
    // заводить абзац незачем.
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"ru-RU",
                             &statusFormat_);
    if (statusFormat_) {
        statusFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        statusFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

Grid BookView::buildTree() {
    // Словарь синтаксиса нужен ровно здесь — и вносится ровно здесь: теги
    // называются как свойства (width, height, margin), и на уровне файла они
    // перекрыли бы одноимённые переменные во всём остальном коде.
    using namespace wxl::dsl;

    // Два листа под общим контейнером. Клип контейнера — то, что не даёт
    // повёрнутому листу вылезти за полосу: клип живёт в координатах самого
    // визуала и применяется до его преобразования, поэтому повёрнутый лист
    // обрезается по прямоугольнику страницы, а не по описанному вокруг него.
    sheets_ = compositor_.createContainerVisual();
    sheets_.value().clip(compositor_.createInsetClip());

    for (int index = 0; index < 2; ++index) {
        SpriteVisual sheet = compositor_.createSpriteVisual();

        // Крой заводится сразу, а не в начале переворота: заводить его там
        // было бы то же самое, только каждый раз заново. В покое он ничего не
        // режет — отступы отрицательные, и окно шире листа ровно на вылет
        // тени, которую иначе срезало бы вместе с ней.
        InsetClip crop = compositor_.createInsetClip(-kShadowReach, -kShadowReach, -kShadowReach,
                                                     -kShadowReach);
        sheet.clip(crop);

        sheets_.value().children().insertAtTop(sheet);
        sheet_.push_back(sheet);
        clip_.push_back(crop);
    }

    // Тень поднятой бумаги. Кисть у неё горизонтальная и в долях собственной
    // ширины (MappingMode по умолчанию относительный), поэтому полоске
    // достаточно ездить — перекрашивать её не приходится.
    CompositionLinearGradientBrush foldBrush = compositor_.createLinearGradientBrush();
    foldBrush.startPoint({0.0f, 0.0f});
    foldBrush.endPoint({1.0f, 0.0f});
    CompositionColorGradientStop foldMid =
        compositor_.createColorGradientStop(kFoldMidStop, colors.transparent);
    foldBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    foldBrush.colorStops().append(foldMid);
    foldBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));
    foldMid_ = foldMid;

    SpriteVisual fold = compositor_.createSpriteVisual();
    fold.brush(foldBrush);
    fold.isVisible(false);
    sheets_.value().children().insertAtTop(fold);

    foldBrush_ = foldBrush;
    fold_ = fold;

    // Тень наружного края приходящего листа. Отдельная от предыдущей: у той
    // край поднят и тень широкая, растущая, а этот край лежит на бумаге, и
    // тень у него узкая и неизменная. Градиент развёрнут — густо у листа,
    // прозрачно прочь от него.
    CompositionLinearGradientBrush edgeBrush = compositor_.createLinearGradientBrush();
    edgeBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    edgeBrush.colorStops().append(compositor_.createColorGradientStop(kEdgeMidStop, colors.transparent));
    edgeBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));

    SpriteVisual edge = compositor_.createSpriteVisual();
    edge.brush(edgeBrush);
    edge.isVisible(false);
    sheets_.value().children().insertAtTop(edge);

    edgeBrush_ = edgeBrush;
    edge_ = edge;

    // Приходящий лист. Кисть ему выдаётся на каждый переворот — ту же, что
    // носит лежащий внизу разворот, — а крой у него свой: им он и выезжает.
    SpriteVisual leaf = compositor_.createSpriteVisual();
    InsetClip leafCrop = compositor_.createInsetClip();
    leaf.clip(leafCrop);
    leaf.isVisible(false);
    sheets_.value().children().insertAtTop(leaf);

    // Полутон изгиба — ребёнок листа, а не сосед: он затеняет саму бумагу и
    // обязан кроиться вместе с ней. Дети рисуются поверх кисти визуала, так
    // что ложится он именно на страницу.
    CompositionLinearGradientBrush bendBrush = compositor_.createLinearGradientBrush();
    bendBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    bendBrush.colorStops().append(compositor_.createColorGradientStop(kBendMidStop, colors.transparent));
    bendBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));

    SpriteVisual bend = compositor_.createSpriteVisual();
    bend.brush(bendBrush);
    leaf.children().insertAtTop(bend);

    bendBrush_ = bendBrush;
    bend_ = bend;

    leaf_ = leaf;
    leafCrop_ = leafCrop;

    applyShadowTint();

    // Страница — на сцене окна: её визуалы висят на композиторе окна и привешены
    // к contentVisual() (над задником, под островом), а не всунуты в дерево XAML
    // через setElementChildVisual. Пока полоса не стала текущим экраном, её сцена
    // скрыта — setActive(true) покажет её при входе в чтение.
    window_->contentVisual().children().insertAtTop(sheets_.value());
    sheets_.value().isVisible(false);

    auto tree = Grid{
        // Корень берёт фокус на себя: событие клавиши начинается у того, на
        // чём фокус, и пока фокуса нет ни на чём, ловить нечего.
        isTabStop = true,

        // Кисть прозрачная, но она есть: без кисти Grid не участвует в проверке
        // попадания вовсе, и щелчок по полосе не доходил бы никуда — ни до знака
        // сноски, ни до трети страницы. Прозрачная потому, что страница теперь
        // на сцене под этим островом, и сквозь него должна быть видна она.
        // Бумагу-основу под страницей несёт задник окна (см. setActive), а сам
        // набор нарисован на сцене.
        background = SolidColorBrush{colors.transparent},

        note_.root(),
    };

    tree.add_onLoaded([this](Object const&, RoutedEventArgs&) {
        root_.value().focus(FocusState::Programmatic);
        if (resizeSurface()) relayoutNow();
    });

    tree.add_onSizeChanged([this](Object const&, SizeChangedEventArgs&) {
        // Единственное место, где нужен гистерезис: окно тянут мышью, граница
        // меры проходит под курсором, и без него колонки защёлкают. Знака
        // хватает — дрожь бывает в доли знака, а не в четыре.
        if (resizeSurface()) requestRelayout(true);
    });

    tree.add_onPreviewKeyDown([this](Object const&, KeyRoutedEventArgs& args) {
        // Поверх полосы лежит мастер, но листание остаётся: изгибы
        // подстраивают под конкретный текст, и ходить по книге нужно прямо
        // из него. Всё остальное — Enter, Escape, тема — мастера.
        if (preview_) {
            switch (args.key()) {
                case VirtualKey::PageDown:
                case VirtualKey::Right:
                case VirtualKey::Down:
                case VirtualKey::Space:
                    turnPage(1);
                    break;
                case VirtualKey::PageUp:
                case VirtualKey::Left:
                case VirtualKey::Up:
                    turnPage(-1);
                    break;
                case VirtualKey::Home:
                    if (book_) goToCharOffset(0);
                    break;
                case VirtualKey::End:
                    if (book_) goToCharOffset(book_->characterCount());
                    break;
                default: return;
            }
            args.handled(true);
            return;
        }
        switch (args.key()) {
            case VirtualKey::PageDown:
            case VirtualKey::Right:
            case VirtualKey::Down:
            case VirtualKey::Space:
                turnPage(1);
                break;
            case VirtualKey::PageUp:
            case VirtualKey::Left:
            case VirtualKey::Up:
                turnPage(-1);
                break;
            case VirtualKey::Home:
                if (book_) goToCharOffset(0);
                break;
            case VirtualKey::End:
                // Конец книги известен только досчитанной, поэтому здесь
                // чистовой набор доводится до самого конца.
                if (book_) goToCharOffset(book_->characterCount());
                break;
            case VirtualKey::Add:
                if (controlHeld()) setFontSize(fontSize_ + 1.0f);
                break;
            case VirtualKey::Subtract:
                if (controlHeld()) setFontSize(fontSize_ - 1.0f);
                break;
            case VirtualKey::Number0:
            case VirtualKey::NumberPad0:
                if (controlHeld()) setFontSize(20.0f);
                break;
            case VirtualKey::T:
                // Голая T меняет тему; Ctrl+T -- оглавление, и его разбирает
                // приложение: сюда оно не должно доходить вовсе.
                if (controlHeld()) return;
                setTheme(theme_ + 1);
                break;
            default:
                return;   // не наша клавиша: пусть идёт дальше
        }
        args.handled(true);
    });

    tree.add_onPointerWheelChanged([this](Object const&, PointerRoutedEventArgs& args) {
        // Работает и под мастером: колесо листает, а Ctrl с колесом меняет
        // кегль — изгиб подстраивают под конкретный текст в конкретном виде.
        const int delta = args.getCurrentPoint(root_.value()).properties().mouseWheelDelta();
        const bool control = (static_cast<uint32_t>(args.keyModifiers()) &
                              static_cast<uint32_t>(VirtualKeyModifiers::Control)) != 0;

        if (control) {
            setFontSize(fontSize_ + (delta > 0 ? 1.0f : -1.0f));
        } else {
            turnPage(delta > 0 ? -1 : 1);
        }
        args.handled(true);
    });

    tree.add_onPointerPressed([this](Object const&, PointerRoutedEventArgs& args) {
        const PointerPoint touch = args.getCurrentPoint(root_.value());
        const Point point = touch.position();

        // Поверх полосы лежит мастер: из всего щелчка полосе остаётся
        // листание по третям — ни ящика, ни сносок, ни фокуса. Сюда доходят
        // только щелчки мимо точек сетки: попавшие мастер разобрал сам.
        if (preview_) {
            if (!touch.properties().isLeftButtonPressed()) return;
            const float third = width_ / 3.0f;
            if (point.x < third) {
                turnPage(-1);
            } else if (point.x > width_ - third) {
                turnPage(1);
            }
            return;
        }
        root_.value().focus(FocusState::Programmatic);
        args.handled(true);

        // Правая кнопка — единственная дорога к ящику для того, кто держит
        // мышь: у страницы нет ни полосы меню, ни кнопок, и заводить их ради
        // этого значило бы завесить книгу обстановкой.
        if (touch.properties().isRightButtonPressed()) {
            if (onPanelRequested) onPanelRequested();
            return;
        }

        // Знак сноски важнее перелистывания: он мелкий, и промах по нему из-за
        // того, что страница уже перевернулась, читателя злит.
        Point anchor;
        if (const fb3::Node* mark = noteAt(point, anchor)) {
            note_.show(*book_, mark, anchor, {width_, height_}, paper(), fontSize_, scale_);
            return;
        }

        // Щелчок мимо знака закрывает то, что открыто поверх полосы: читатель
        // прочёл примечание и вернулся к книге.
        if (note_.visible()) {
            note_.hide();
            return;
        }

        // Щелчок по левой трети полосы — назад, по правой — вперёд. Середина
        // не делает ничего: там текст, и промах по ссылке не должен листать.
        const float third = width_ / 3.0f;
        if (point.x < third) {
            turnPage(-1);
        } else if (point.x > width_ - third) {
            turnPage(1);
        }
    });

    return tree;
}

void BookView::addOverlay(const UIElement& element) {
    root_.value().children().append(element);
}

void BookView::open(std::shared_ptr<Book> book, std::uint32_t charOffset) {
    note_.hide();
    book_ = std::move(book);
    readingPosition_ = charOffset;
    page_ = 0;
    requestRelayout();
}

std::span<const typography::Block> BookView::blocks() const {
    if (!book_) return {};
    return book_->blocks();
}

std::size_t BookView::pageCount() const {
    return book_ ? book_->paginator().pageCount() : 0;
}

float BookView::progress() const {
    if (!book_ || book_->characterCount() == 0) return 0.0f;
    return std::clamp(
        static_cast<float>(readingPosition_) / static_cast<float>(book_->characterCount()), 0.0f,
        1.0f);
}

bool BookView::dismissOverlays() {
    if (!note_.visible()) return false;
    note_.hide();
    return true;
}

void BookView::setActive(bool active) {
    active_ = active;

    // Задник окна в чтении — сама страница, один битмап на весь задник: её
    // ставит redraw поверхностью surface_[resting_], и никакой бумаги-подложки
    // под ней нет. Уходя, задник вернёт себе стартовый экран (заставку).
    if (active && window_) {
        updateBackdrop();
        redraw();
    }

    // Листы в покое не нужны: страница видна задником окна. Они выходят на сцену
    // только на время переворота — показывает их startTurn, прячут обратно
    // turnCompleted и cancelTurn.
    if (sheets_) sheets_.value().isVisible(false);
}

void BookView::setTheme(int index) {
    const int count = themeCount();
    theme_ = ((index % count) + count) % count;
    note_.hide();   // подложка всплывашки покрашена прошлой темой
    // Бумага-основа — задником окна, а не кистью корня: страница теперь на
    // сцене, и основа под ней, сквозь которую при растяжке видна бумага, а не
    // заставка, — это задний фон окна. Ставим только когда полоса показана: до
    // входа в чтение задником владеет заставка стартового экрана.
    if (active_ && window_) window_->background(ARGB{argbOf(paper().background)});
    applyShadowTint();   // тени тоже покрашены прошлой темой

    // Карта держит форму конкретной обложки — новой теме она не годится.
    warpMap_.Reset();
    warpFlat_ = false;

    // Слой изгиба нужен только теме с подложкой, а весит как две полосы —
    // на ровных темах он отпускается. Контекст и эффекты мелкие и остаются.
    if (backdropFile().empty()) {
        warpLayer_.Reset();
        warpPixels_ = {};
    }
    updateBackdrop();
    redraw();
}

void BookView::setSkins(std::vector<Skin> skins) {
    skins_ = std::move(skins);

    // Реестр мог и похудеть: тема, показывающая исчезнувшую обложку, честно
    // возвращается к первой встроенной.
    if (theme_ >= themeCount()) theme_ = 0;

    // Пересохранённая обложка могла сменить и снимок, и кривые.
    backdropLoaded_.clear();
    backdropSource_.Reset();
    photoBitmap_.Reset();
    warpMap_.Reset();
    warpFlat_ = false;
    updateBackdrop();
    redraw();
}

const Skin* BookView::activeSkin() const {
    if (theme_ < kThemeCount) return nullptr;
    return &skins_[static_cast<std::size_t>(theme_ - kThemeCount)];
}

void BookView::setPreview(const Skin* skin, const std::filesystem::path& image) {
    if (skin) {
        preview_ = *skin;
        previewImage_ = image;
    } else {
        preview_.reset();
        previewImage_.clear();
    }

    // Карта держит форму прежних кривых, а подложка — прежний снимок.
    warpMap_.Reset();
    warpFlat_ = false;
    updateBackdrop();
    redraw();
}

std::filesystem::path BookView::backdropFile() const {
    if (preview_) return previewImage_;
    if (const Skin* skin = activeSkin()) return skinDirectory() / skin->image;
    if (paper().backdrop) return exeDirectory() / paper().backdrop;
    return {};
}

void BookView::setFontSize(float size) {
    const float wanted = std::clamp(size, 10.0f, 48.0f);
    if (wanted == fontSize_) return;
    fontSize_ = wanted;
    requestRelayout();
}

void BookView::setLineHeight(float multiplier) {
    const float wanted = std::clamp(multiplier, 1.0f, 2.4f);
    if (wanted == lineHeight_) return;
    lineHeight_ = wanted;
    requestRelayout();
}

void BookView::setMargin(float fraction) {
    const float wanted = std::clamp(fraction, 0.02f, 0.25f);
    if (wanted == marginFraction_) return;
    marginFraction_ = wanted;
    requestRelayout();
}

void BookView::prepare(float width, float height, float scale) {
    // Полоса верстается и рисуется до того, как её покажут.
    //
    // Размер для этого известен заранее: полоса занимает окно целиком, а
    // размер окна и масштаб экрана можно спросить у того, что показано сейчас,
    // — окно-то одно. Без этого читатель, нажав «Продолжить чтение», успевает
    // увидеть пустой лист: элемент попадает в дерево сразу, а рисовать его
    // есть чем только со следующего кадра.
    if (applySize(width, height, scale)) relayoutNow();
}

bool BookView::resizeSurface() {
    const auto width = static_cast<float>(root_.value().actualWidth());
    const auto height = static_cast<float>(root_.value().actualHeight());

    // Масштаб экрана берётся у XamlRoot, а не считается от DPI окна: это то
    // же число, которым XAML умножает DIP в пиксели, и оно обязано совпадать.
    // XamlRoot появляется, когда элемент попал в живое дерево: до Loaded его
    // нет, и спрашивать масштаб не у кого.
    Nullable<XamlRoot> const xamlRoot = root_.value().xamlRoot();
    float scale = xamlRoot ? static_cast<float>(xamlRoot->rasterizationScale()) : 1.0f;
    if (scale <= 0.0f) scale = 1.0f;

    return applySize(width, height, scale);
}

bool BookView::applySize(float width, float height, float scale) {
    if (width == width_ && height == height_ && scale == scale_ && !surface_.empty()) return false;

    width_ = width;
    height_ = height;
    scale_ = scale;

    const SizeInt32 pixels{static_cast<int32_t>(width * scale + 0.5f),
                           static_cast<int32_t>(height * scale + 0.5f)};
    if (pixels.width <= 0 || pixels.height <= 0) return false;

    // Поверхности меняют размер, а не пересоздаются: кисти, которые их уже
    // показывают, продолжают показывать их же.
    if (surface_.empty()) {
        for (std::size_t index = 0; index < sheet_.size(); ++index) {
            surface_.emplace_back(compositor_, pixels);
            sheet_[index].brush(surface_[index].brush());

            // Тень листа — здесь, а не в начале переворота: маской ей служит
            // кисть листа, а кисть появляется ровно тут. Дальше она только
            // гаснет и зажигается.
            DropShadow shadow = compositor_.createDropShadow();
            shadow.blurRadius(kShadowBlur);
            shadow.offset({kShadowShift, 0.0f, 0.0f});
            shadow.mask(sheet_[index].brush());
            shadow.opacity(0.0f);
            sheet_[index].shadow(shadow);
            shadow_.push_back(shadow);
        }
    } else {
        for (DrawingSurface& sheet : surface_) sheet.resize(pixels);
    }

    for (SpriteVisual const& sheet : sheet_) {
        sheet.size({width, height});
        // Поворот идёт вокруг левого края: там у книги корешок, и лист
        // поднимается именно оттуда.
        sheet.centerPoint({0.0f, height * 0.5f, 0.0f});
    }
    sheets_.value().size({width, height});

    // Полоска тени меряется целым разворотом, а до нужной доли её ужимает
    // Scale: ширина тени зависит от корешка, а корешок при этом вызове ещё
    // может быть не посчитан.
    fold_.value().size({width, height});

    // А этой ужимать нечего: ширина у неё постоянная, и посчитать её можно
    // прямо здесь.
    edge_.value().size({width * kEdgeOfWindow, height});
    bend_.value().size({width * kBendOfWindow, height});

    // Приходящий лист меряется целым разворотом, как и обычные листы: он
    // носит их кисть, и страница на ней лежит в тех же координатах.
    leaf_.value().size({width, height});

    applyShadowTint();   // у теней, заведённых выше, цвет ещё не темы

    // Окно кроя задано в тех же единицах, что и полоса, и после смены её
    // размера бессмысленно. Оставить его недосброшенным — значит спрятать
    // кусок настоящей страницы, если размер сменился посреди переворота.
    // Вместе с ним отменяется и очередь: доигрывать её по новым размерам
    // означало бы листать вслепую.
    cancelTurn();
    resetSheets();
    return true;
}

void BookView::requestRelayout(bool windowResize) {
    // Прямо здесь, в обработчике события: грязная страница стоит единицы
    // миллисекунд, и откладывать её значило бы показать читателю его же
    // движение мыши с опозданием на кадр без всякой на то причины.
    windowResize_ = windowResize;
    relayoutNow();
}

float BookView::characterWidth() const {
    if (!book_) return fontSize_ * 0.5f;

    const typography::TextStyle& text = book_->engine().textStyle();

    // Мерить каждый раз незачем: ответ зависит только от шрифта и кегля, а
    // спрашивают его на каждой перевёрстке.
    if (charWidth_ > 0.0f && charFontSize_ == fontSize_ && charFamily_ == text.fontFamily) {
        return charWidth_;
    }

    IDWriteFactory* const dwrite = dwriteFactory();
    if (!dwrite) return fontSize_ * 0.5f;

    // Латиница или кириллица — по языку книги: средняя ширина знака у них
    // разная, и мерить английскую фразу для русской книги значило бы мерить
    // не то.
    const bool cyrillic = text.locale.starts_with(L"ru");
    const std::wstring_view sample = cyrillic ? std::wstring_view{kCyrillicSample}
                                              : std::wstring_view{kLatinSample};

    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(text.fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        fontSize_, text.locale.c_str(), format.GetAddressOf()))) {
        return fontSize_ * 0.5f;
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(sample.data(), static_cast<UINT32>(sample.size()),
                                        format.Get(), 1.0e6f, 1.0e6f, layout.GetAddressOf()))) {
        return fontSize_ * 0.5f;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)) || metrics.width <= 0.0f) return fontSize_ * 0.5f;

    charWidth_ = metrics.width / static_cast<float>(sample.size());
    charFontSize_ = fontSize_;
    charFamily_ = text.fontFamily;
    return charWidth_;
}

float BookView::lineChars(int columns) const {
    const float margin = width_ * marginFraction_;
    const float available = width_ - margin * 2.0f;
    const float gutters = margin * kGutterOfMargin * static_cast<float>(columns - 1);
    return (available - gutters) / static_cast<float>(columns) / characterWidth();
}

int BookView::chooseColumns(bool sticky) const {
    const float margin = width_ * marginFraction_;
    if (width_ - margin * 2.0f <= 0.0f) return 1;

    // Колонки заполняют место между полями целиком, поэтому мера решает
    // единственный вопрос — сколько их. Добавляем колонку, пока строка длиннее
    // меры и пока следующая колонка не выйдет слишком узкой: на мелком кегле
    // широкое окно — это не одна строка в двести знаков, а три по семьдесят.
    int wanted = 1;
    while (lineChars(wanted) > kMaxLineChars && lineChars(wanted + 1) >= kMinLineChars) {
        ++wanted;
    }

    if (sticky) {
        // Держимся за нынешнее число, пока оно не стало откровенно плохим.
        if (wanted > columns_ && lineChars(columns_) <= kMaxLineChars + kColumnHysteresis) {
            return columns_;
        }
        if (wanted < columns_ && lineChars(columns_) >= kMinLineChars - kColumnHysteresis) {
            return columns_;
        }
    }
    return wanted;
}

void BookView::relayoutNow() {
    // Всякая новая вёрстка отменяет чистовой набор, который шёл: считать
    // книгу по прежней полосе больше незачем, а его порции, дождавшись
    // очереди, увидят чужой номер и разойдутся.
    ++paginationEpoch_;

    // Перевёрстка меняет и корешок, и ширину страницы, а листы могли остаться
    // от прерванного переворота с окном кроя, посчитанным по прежним числам.
    // Очередь листания к прежним числам привязана не меньше: и номер разворота
    // в ней, и число колонок, которым он считается, после перевёрстки другие.
    cancelTurn();
    resetSheets();

    if (!book_ || width_ <= 0.0f || height_ <= 0.0f) {
        redraw();
        return;
    }

    // Пагинатор верстает по одной главе: наводим его на ту, где стоит читатель,
    // прежде чем считать. Та же глава — вызов ничего не делает, и тяга кегля не
    // пере-шейпит; другая (открыли книгу на запомненном месте) — глава
    // посчитается заново.
    book_->setCurrentChapter(readingPosition_);

    const float margin = width_ * marginFraction_;
    const float statusHeight = fontSize_ * 1.6f;

    columns_ = chooseColumns(windowResize_);
    windowResize_ = false;

    const float gutters = margin * kGutterOfMargin * static_cast<float>(columns_ - 1);
    const float share = (width_ - margin * 2.0f - gutters) / static_cast<float>(columns_);

    typography::PageStyle style;
    // Ширину полосы задают поля, и ничто больше: место между ними делится
    // между колонками поровну. Свой предел здесь стоял бы поперёк ползунка
    // «Поля» — читатель просит колонку уже, а она не слушается.
    style.width = std::max(share, fontSize_ * 8.0f);
    style.height = std::max(height_ - kVerticalMargin * 2.0f - statusHeight, fontSize_ * 4.0f);
    style.fontSize = fontSize_;
    style.lineHeight = lineHeight_;

    pageStyle_ = style;

    // Вот ради чего книга режется на главы: полоса стала другой, а перевёрстка
    // считает не всю книгу, а одну текущую главу — единицы миллисекунд, — и
    // потому идёт начисто прямо здесь, в обработчике события. Черновика больше
    // нет: с разбивкой по главам чистовой набор сам достаточно дёшев.
    //
    // Досчитываем ровно до видимого разворота — этого хватает, чтобы показать
    // страницу; остаток главы добирается порциями в простое, и с него
    // становится известно общее число страниц («из M»).
    typography::Chapter& paginator = book_->paginator();
    paginator.beginLayout(pageStyle_);
    paginator.advanceTo(readingPosition_);

    // Место чтения становится левой колонкой разворота — разворот начинается
    // ровно с той колонки, где лежала буква, а не с округлённого вниз края.
    // Так на стыке глав не пропадает колонка: лента идёт от места чтения
    // подряд, и правую сторону разворота при нужде занимает начало следующей
    // главы. Прижимать читателя к началу колонки нестрашно — колонки при одном
    // стиле бьются одинаково, так что прищёлк случается лишь однажды, на смене
    // кегля, а не уезжает с каждой перевёрсткой.
    page_ = paginator.pageCount() == 0 ? 0 : paginator.pageForCharOffset(readingPosition_);
    paginator.advanceToPage(page_ + static_cast<std::size_t>(columns_));
    if (paginator.pageCount() != 0)
        readingPosition_ = paginator.page(page_).firstCharOffset;

    redraw();

    // Остаток главы — порциями в свободное время потока: с него узнаётся общее
    // число страниц. Если глава уже досчиталась (короткая), звать нечего.
    if (!paginator.isComplete())
        startPagination();
}

void BookView::startPagination() {
    // Заказ уже в очереди — второй ничего не прибавит: задание всё равно
    // продолжит счёт той главы, какую застанет.
    if (cleanPosted_) return;
    cleanPosted_ = true;

    std::weak_ptr<int> alive = alive_;

    // Тот же набор, что relayoutNow уже начал и досчитал до видимого разворота:
    // его номер — нынешний paginationEpoch_. Порции продолжают счёт с курсора
    // главы, не начиная заново, — синхронно посчитанные страницы остаются на
    // месте, а фон лишь добирает хвост главы ради общего числа страниц.
    const std::uint32_t epoch = paginationEpoch_;

    // Низкий приоритет — это и есть «в свободное время»: поток сперва разберёт
    // ввод и покажет нарисованное, а уже потом возьмётся за книгу.
    queue_.tryEnqueue(DispatcherQueuePriority::Low, [this, alive, epoch] {
        if (alive.expired()) return;
        cleanPosted_ = false;
        if (!book_) return;
        paginateChunk(epoch);
    });
}

void BookView::paginateChunk(std::uint32_t epoch) {
    if (epoch != paginationEpoch_ || !book_) return;

    const bool more = book_->paginator().advance(kPaginationSlice);

    // Порции добирают хвост главы, а не то, что видно: видимый разворот уже
    // посчитан начисто в relayoutNow. Меняется от них лишь общее число страниц
    // — и то один раз, когда глава досчитана. До тех пор колонцифра показывает
    // «из …», поэтому и перерисовывать нечего, пока счёт идёт.
    if (more) {
        std::weak_ptr<int> alive = alive_;
        queue_.tryEnqueue(DispatcherQueuePriority::Low, [this, alive, epoch] {
            if (alive.expired()) return;
            paginateChunk(epoch);
        });
        return;
    }

    redraw();   // глава досчитана: «из …» стало «из M»
}

BookView::Column BookView::columnOf(std::uint32_t charOffset) {
    // Место может лежать в другой главе — наводим на неё и верстаем начисто.
    book_->setCurrentChapter(charOffset);

    typography::Chapter& paginator = book_->paginator();
    if (paginator.pageCount() == 0)
        paginator.beginLayout(pageStyle_);

    // Энергично, без срока: читатель прыгнул по закладке и ждёт ответа. Остаток
    // главы по-прежнему добирается порциями — та, что стоит в очереди, продолжит
    // с того, на чём мы кончили.
    paginator.advanceTo(charOffset);
    if (paginator.pageCount() == 0)
        return Column{book_->currentChapter(), 0};

    // Колонка, в которой лежит символ, — левая колонка разворота. К числу
    // колонок не прижимаем: разворот начинается ровно с места чтения, а не с
    // округлённого вниз края, — иначе на стыке глав пропадала бы колонка.
    return Column{book_->currentChapter(), paginator.pageForCharOffset(charOffset)};
}

BookView::Column BookView::anchorColumn() const {
    const std::size_t chapter =
        book_->currentChapter() == static_cast<std::size_t>(-1) ? 0 : book_->currentChapter();
    return Column{chapter, page_};
}

typography::Chapter& BookView::chapterLaidTo(std::size_t index, std::size_t pages) {
    typography::Chapter& chapter = book_->chapterAt(index);

    // Разложена ли она под нынешнюю полосу? Свежая (ни одной страницы) или
    // соседняя, оставшаяся в кэше от прежней полосы, — переложить под текущую.
    // Текущую главу это не трогает: её стиль уже совпадает.
    if (chapter.pageCount() == 0 || !(chapter.style() == pageStyle_))
        chapter.beginLayout(pageStyle_);
    chapter.advanceToPage(pages);
    return chapter;
}

bool BookView::ribbonStep(Column& pos, bool forward) {
    if (forward) {
        // В пределах главы — следующая колонка, если она есть. advanceToPage до
        // pos.index+2 доводит счёт настолько, чтобы знать: либо колонка есть,
        // либо глава на ней и кончилась (тогда она уже complete).
        typography::Chapter& chapter = chapterLaidTo(pos.chapter, pos.index + 2);
        if (pos.index + 1 < chapter.pageCount()) {
            ++pos.index;
            return true;
        }
        // Глава кончилась — на начало первой непустой следующей.
        for (std::size_t next = pos.chapter + 1; next < book_->chapterCount(); ++next) {
            if (chapterLaidTo(next, 1).pageCount() > 0) {
                pos.chapter = next;
                pos.index = 0;
                return true;
            }
        }
        return false;   // последняя колонка книги
    }

    if (pos.index > 0) {
        --pos.index;
        return true;
    }
    // Начало главы — в конец предыдущей непустой. Её нужно знать целиком, чтобы
    // взять последнюю колонку, — верстаем до конца (глава мала).
    for (std::size_t prev = pos.chapter; prev-- > 0;) {
        typography::Chapter& chapter = chapterLaidTo(prev, static_cast<std::size_t>(-1));
        if (chapter.pageCount() > 0) {
            pos.chapter = prev;
            pos.index = chapter.pageCount() - 1;
            return true;
        }
    }
    return false;   // первая колонка книги
}

bool BookView::ribbonSpread(Column& pos, bool forward) {
    Column probe = pos;
    for (int i = 0; i < columns_; ++i) {
        if (!ribbonStep(probe, forward)) {
            if (forward)
                return false;         // конец книги — разворот не сдвинуть
            probe = Column{};         // начало книги — на самый первый разворот
            break;
        }
    }
    if (probe.chapter == pos.chapter && probe.index == pos.index)
        return false;
    pos = probe;
    return true;
}

void BookView::buildSpread() {
    spread_.clear();
    spreadOwnColumns_ = 0;
    if (!book_ || width_ <= 0.0f)
        return;

    // Кэш держим ровно вокруг текущей главы. Радиус обязан покрыть весь
    // показанный разворот: он тянется на несколько глав вперёд, если они короче
    // него. Чистим до сборки — иначе трим уронил бы главу, чью страницу лента
    // уже держит.
    book_->trimChapters(static_cast<std::size_t>(columns_) + 1);

    const Column anchor = anchorColumn();
    Column pos = anchor;

    for (int slot = 0; slot < columns_; ++slot) {
        // Довести колонку до реальной страницы, перешагивая исчерпанные главы:
        // короткая глава бывает уже разворота, и на неё приходится не одна его
        // колонка.
        const typography::Page* page = nullptr;
        while (pos.chapter < book_->chapterCount()) {
            typography::Chapter& chapter = chapterLaidTo(pos.chapter, pos.index + 1);
            if (pos.index < chapter.pageCount()) {
                page = &chapter.page(pos.index);
                break;
            }
            ++pos.chapter;   // в этой главе такой колонки нет — на начало следующей
            pos.index = 0;
        }
        if (!page)
            break;   // конец книги — дальше пусто

        spread_.push_back(page);
        if (pos.chapter == anchor.chapter)
            ++spreadOwnColumns_;
        ++pos.index;
    }
}

void BookView::showColumn(const Column& target) {
    cancelTurn();
    note_.hide();

    if (target.chapter != book_->currentChapter())
        book_->makeCurrentChapter(target.chapter);
    page_ = target.index;
    if (book_->paginator().pageCount() != 0)
        readingPosition_ = book_->paginator().page(page_).firstCharOffset;

    redraw();
    if (onPositionChanged) onPositionChanged(readingPosition_);
}

void BookView::redraw() {
    if (surface_.empty() || width_ <= 0.0f || height_ <= 0.0f) return;

    // Собираем показанный разворот из ленты колонок до отрисовки: дальше и
    // рисование, и попадание по сноске читают уже готовый spread_.
    buildSpread();

    surface_[resting_].draw([this](ID2D1DeviceContext* context) {
        // Поверхность в пикселях, а вёрстка в DIP: масштаб домножается к тому
        // смещению атласа, которое wxl уже поставила, — иначе поверхность
        // легла бы поверх чужой.
        D2D1_MATRIX_3X2_F atlas{};
        context->GetTransform(&atlas);
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_) *
                              *D2D1::Matrix3x2F::ReinterpretBaseType(&atlas));

        // Классический ClearType здесь не годится: он рассчитан на неподвижный
        // текст на непрозрачном фоне и рассыпается на цветную бахрому при
        // повороте, дробном масштабе и на OLED. Серая сглаженность с
        // субпиксельным позиционированием выглядит одинаково везде.
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (!spread_.empty()) {
            drawPage(context, width_, height_);
        } else {
            drawInvitation(context, width_, height_);
        }
    });

    // Страница — задник окна: один битмап на весь задник, кроет его синхронно,
    // как заставка. В чтении (active_) ставим поверхность задником; на стартовом
    // экране задником владеет заставка.
    if (active_ && window_) window_->background(surface_[resting_]);
}

void BookView::turnPage(int delta) {
    if (!book_ || pageCount() == 0 || delta == 0)
        return;
    const bool forward = delta > 0;

    // Встречное листание отменяет очередь: показывать дорогу туда, откуда
    // читатель уже повернул назад, незачем. Тогда шаг считается от видимого
    // разворота, а не от конца прежней очереди.
    if (turning_ && forward != turnForward_)
        cancelTurn();

    // Конец очереди — колонка, до которой дойдёт последний заказанный переворот.
    // От неё и считается следующий разворот ленты. Лента непрерывна, так что
    // это обычный шаг: границу главы он проходит сам, не прыжком.
    Column target = queueEnd();
    if (!ribbonSpread(target, forward))
        return;   // край книги — листать некуда

    // Тот же ход направления при идущем перевороте — в очередь; иначе новый.
    // Плоское листание turning_ не держит, там всегда startTurn.
    if (turning_) {
        ++pending_;
        return;
    }
    startTurn(target, forward);
}

BookView::Column BookView::queueEnd() {
    Column pos = anchorColumn();
    // Каждый заказанный, но ещё не начатый переворот сдвигает конец очереди на
    // разворот в сторону turnForward_. В очередь попадает только тот шаг, что в
    // книге есть (см. turnPage), поэтому все они проходят.
    for (int i = 0; i < pending_; ++i)
        ribbonSpread(pos, turnForward_);
    return pos;
}

void BookView::startTurn(const Column& target, bool forward) {
    note_.hide();   // страница ушла, а сноска на ней осталась бы висеть

    // Целевая колонка может лежать в соседней главе — делаем её текущей, не
    // теряя вёрстки: buildSpread уже разложил её как соседнюю на стыке.
    if (target.chapter != book_->currentChapter())
        book_->makeCurrentChapter(target.chapter);
    page_ = target.index;
    readingPosition_ = book_->paginator().page(page_).firstCharOffset;

    // Новая страница рисуется на свободный лист, и он становится тем, на
    // котором книга стоит. Порядок именно такой: к началу анимации верная
    // страница уже нарисована и уже лежит внизу, поэтому сбой анимации может
    // стоить кадра, но не страницы. Отсюда же и то, почему очередь не забегает
    // вперёд: свободный лист один, и пока по нему едет кромка, рисовать
    // следующий разворот некуда, — потому очередь и хранится числом шагов.
    resting_ = 1 - resting_;
    redraw();

    // Листы выходят на сцену на время переворота — в покое их прячут; страница
    // при этом уже задником окна (redraw), и уезжающий лист открывает её.
    if (sheets_) sheets_.value().isVisible(true);

    // Книжное листание — только для разворота: снимать бумагу с корешка можно
    // там, где корешок есть. В одну колонку и в три листается тем же, чем
    // листалось всегда, и очереди там нет — новая анимация перебивает старую.
    if (columns_ == 2) {
        animateSpreadTurn(forward);
    } else {
        animateTurn(forward);
    }

    if (onPositionChanged) onPositionChanged(readingPosition_);
}

void BookView::turnCompleted() {
    turning_ = false;
    if (pending_ == 0) {
        if (sheets_) sheets_.value().isVisible(false);   // очередь пуста — в покой
        return;
    }

    --pending_;
    Column next = anchorColumn();
    if (!ribbonSpread(next, turnForward_)) {   // упёрлись в край — очередь оборвана
        pending_ = 0;
        if (sheets_) sheets_.value().isVisible(false);
        return;
    }
    startTurn(next, turnForward_);
}

void BookView::cancelTurn() {
    // Номер меняется — и обработчик конца, если пакет отменённого переворота
    // всё-таки о нём скажет, узнает свой номер чужим и промолчит.
    ++turnEpoch_;
    turning_ = false;
    pending_ = 0;
    // Переворот отменён — в покой: листы прячем, страница видна задником окна.
    // Начнётся следом встречный переворот — startTurn снова их покажет.
    if (sheets_) sheets_.value().isVisible(false);
}

void BookView::raise(const SpriteVisual& sheet) {
    // Сначала вынуть, потом положить наверх. Композитор не переставляет визуал,
    // у которого уже есть родитель, — он отвечает на это E_INVALIDARG, а
    // исключение из обработчика XAML стоит приложению жизни. Оба листа лежат
    // в контейнере с самого начала, так что «уже есть» — это всегда.
    sheets_.value().children().remove(sheet);
    sheets_.value().children().insertAtTop(sheet);
}

void BookView::applyShadowTint() {
    const D2D1_COLOR_F& hue = paper().shadow;

    // Все кисти теней устроены одинаково: гуще всего у того, кто тень
    // отбрасывает, потом середина спада, потом ничего. Цвет во всех трёх
    // остановках один, разная только прозрачность — в том числе и в последней,
    // где она нулевая: см. tinted().
    auto paint = [&hue](Collection<CompositionColorGradientStop> const& stops,
                        std::uint32_t dense, std::uint32_t middle) {
        if (stops.size() < 3) return;
        stops[0].color(tinted(hue, alphaOf(dense)));
        stops[1].color(tinted(hue, alphaOf(middle)));
        stops[2].color(tinted(hue, 0.0f));
    };

    paint(foldBrush_.value().colorStops(), kFoldNear, kFoldMid);
    paint(edgeBrush_.value().colorStops(), kEdgeNear, kEdgeMid);
    paint(bendBrush_.value().colorStops(), kBendNear, kBendMid);

    // Тень обычного переворота — не градиент, а размытие, и цвет у неё свой
    // собственный. Прозрачностью там правит сама тень, поэтому тон берётся
    // непрозрачным.
    for (DropShadow const& shadow : shadow_) shadow.color(tinted(hue, 1.0f));
}

void BookView::resetSheets() {
    // Сперва всё — на место, и только потом двигается одно.
    //
    // Лист, уехавший в прошлый переворот, там и остаётся: смещение с поворотом
    // ему никто не снимал. Через переворот очередь показывать страницу
    // доходит до него — новая страница честно на нём нарисована, но сам он всё
    // ещё за левым краем, и читатель видит под уезжающей страницей пустоту.
    // Поэтому положение снимается здесь, до анимации, а не в конце прошлой:
    // к первому кадру переворота новая страница обязана уже лежать на месте.
    //
    // Крой тут важнее смещения. Застрявшее смещение портит копию прежней
    // страницы — ту, которой всё равно суждено уехать. Застрявший крой
    // спрятал бы кусок настоящей: того самого листа, на котором книга стоит.
    for (std::size_t index = 0; index < sheet_.size(); ++index) {
        SpriteVisual const& sheet = sheet_[index];

        // Сначала снять анимацию, потом писать. Пока анимация на свойстве
        // жива, прямая запись до него не доходит, и лист, которому не дали
        // доехать при быстром листании, доехал бы уже в роли того, на котором
        // книга стоит, — то есть унёс бы страницу за край.
        sheet.stopAnimation(L"Offset");
        sheet.stopAnimation(L"RotationAngleInDegrees");
        sheet.offset({0.0f, 0.0f, 0.0f});
        sheet.rotationAngleInDegrees(0.0f);

        // В покое крой отпущен на вылет тени: нулевые отступы — это ровно
        // лист, а тени положено лежать за его краем.
        InsetClip const& crop = clip_[index];
        crop.stopAnimation(L"LeftInset");
        crop.stopAnimation(L"RightInset");
        crop.leftInset(-kShadowReach);
        crop.rightInset(-kShadowReach);
        crop.topInset(-kShadowReach);
        crop.bottomInset(-kShadowReach);

        if (index < shadow_.size()) shadow_[index].opacity(0.0f);
    }

    fold_.value().stopAnimation(L"Offset");
    fold_.value().stopAnimation(L"Scale");
    fold_.value().stopAnimation(L"Opacity");
    fold_.value().isVisible(false);
    foldMid_.value().stopAnimation(L"Offset");

    edge_.value().stopAnimation(L"Offset");
    edge_.value().isVisible(false);

    leaf_.value().stopAnimation(L"Offset");
    leaf_.value().isVisible(false);
    bend_.value().stopAnimation(L"Offset");
    bend_.value().stopAnimation(L"Opacity");
    bend_.value().opacity(1.0f);
    leafCrop_.value().stopAnimation(L"LeftInset");
    leafCrop_.value().stopAnimation(L"RightInset");
}

void BookView::animateTurn(bool forward) {
    resetSheets();

    // Уезжает всегда копия прежней страницы, приходит всегда новая. Разница
    // между «вперёд» и «назад» только в том, кто из них наверху: вперёд
    // прежний лист уходит влево, назад новый приходит слева.
    SpriteVisual const& moving = forward ? spare() : resting();
    raise(moving);

    // Уехать надо дальше собственной ширины. Лист поворачивается вокруг левого
    // края, и его дальний нижний угол отходит от оси не на ширину, а на
    // гипотенузу: к ширине, укороченной косинусом, добавляется половина высоты,
    // умноженная на синус. Уезжай лист ровно на ширину — этот угол так и
    // оставался бы в кадре полоской бумаги у левого края.
    // Плюс тень: она сдвинута вправо и размыта, поэтому переживает бумагу и
    // осталась бы у левого края серой полоской, уйди лист ровно по своему углу.
    const float away =
        -(width_ * kTurnCos + height_ * 0.5f * kTurnSin + kShadowShift + kShadowBlur);
    const float from = forward ? 0.0f : away;
    const float to = forward ? away : 0.0f;
    const float angleFrom = forward ? 0.0f : -kTurnAngle;
    const float angleTo = forward ? -kTurnAngle : 0.0f;

    auto const easing = compositor_.createLinearEasingFunction();

    auto slide = compositor_.createVector3KeyFrameAnimation();
    slide.duration(kTurn);
    slide.insertKeyFrame(0.0f, Vector3{from, 0.0f, 0.0f}, easing);
    slide.insertKeyFrame(1.0f, Vector3{to, 0.0f, 0.0f}, easing);

    auto turn = compositor_.createScalarKeyFrameAnimation();
    turn.duration(kTurn);
    turn.insertKeyFrame(0.0f, angleFrom, easing);
    turn.insertKeyFrame(1.0f, angleTo, easing);

    // Тень — не украшение: лист и страница под ним одного цвета, и без тени
    // глаз не видит, что один поднят над другим. Заведена она в applySize,
    // где у листа появляется кисть — она же ей и маска, поэтому тень
    // повторяет лист, а не описанный вокруг прямоугольник.
    shadow_[forward ? 1 - resting_ : resting_].opacity(kShadowOpacity);

    // В конце переворота делать нечего, поэтому конец и не отслеживается:
    // уехавший лист так и остаётся уехавшим до своего следующего выхода, а
    // приводит его в порядок начало следующего переворота — там это нужно, а
    // здесь было бы обещанием, которое некому исполнить, если анимацию
    // прервали.
    moving.startAnimation(L"Offset", slide);
    moving.startAnimation(L"RotationAngleInDegrees", turn);
}

void BookView::animateSpreadTurn(bool forward) {
    resetSheets();

    // Уходит старый разворот, и в обе стороны он остаётся сверху: книжное
    // листание снимает верхнюю бумагу с неподвижной стопки, а не увозит
    // страницу за край. Новый разворот уже нарисован и уже лежит под ней —
    // тем же порядком в startTurn, что и у обычного переворота.
    SpriteVisual const& going = spare();
    InsetClip const& goingCrop = clip_[1 - resting_];
    SpriteVisual const& coming = leaf_.value();
    InsetClip const& comingCrop = leafCrop_.value();
    SpriteVisual const& fold = fold_.value();

    // Приходящий лист носит кисть того самого разворота, что лежит под ним:
    // страница на нём уже нарисована, и нужны от неё только другое место и
    // своё окно. Кисть DrawingSurface делает новую на каждый вызов, а
    // поверхность за ней всё та же — второй отрисовки не возникает.
    coming.brush(surface_[resting_].brush());

    // Пять слоёв по глубине: новый разворот внизу, тень сгиба над ним,
    // снимаемая бумага, тень наружного края приходящего листа — она лежит
    // на снимаемой бумаге, а не на самом листе, — и лист сверху. Каждый
    // слой — вынуть и вставить, по той же причине, что и в raise(): визуал
    // с родителем композитор переставлять отказывается.
    SpriteVisual const& rim = edge_.value();
    VisualCollection const children = sheets_.value().children();
    children.remove(resting());
    children.insertAtBottom(resting());
    children.remove(fold);
    children.insertAbove(fold, resting());
    children.remove(going);
    children.insertAbove(going, fold);
    children.remove(rim);
    children.insertAbove(rim, going);
    children.remove(coming);
    children.insertAtTop(coming);

    const float leftPage = spine();
    const float rightPage = width_ - leftPage;

    // Кромка уходящего листа доходит до корешка: вперёд едет правая, назад —
    // левая.
    const float travel = forward ? rightPage : leftPage;
    const wchar_t* const inset = forward ? L"RightInset" : L"LeftInset";

    // Ровно, без разгона: бумагу тянет рука, а не роняет тяжесть.
    auto const easing = compositor_.createLinearEasingFunction();

    auto crawl = compositor_.createScalarKeyFrameAnimation();
    crawl.duration(kLeafSlide);
    crawl.insertKeyFrame(0.0f, 0.0f, easing);
    crawl.insertKeyFrame(1.0f, travel, easing);

    // Тень едет за кромкой той же прямой и той же длительности. Обе анимации
    // уходят в один коммит, обе — функции нормализованного времени от общего
    // начала, поэтому разойтись им не на чем.
    //
    // Полоска лежит за кромкой: вперёд — справа от неё, назад — слева, и
    // тогда же разворачивается градиент, чтобы гуще было у бумаги.
    const float edge = forward ? width_ : 0.0f;
    const float lead = forward ? 0.0f : -width_;
    const float direction = forward ? 1.0f : -1.0f;

    foldBrush_.value().startPoint({forward ? 0.0f : 1.0f, 0.0f});
    foldBrush_.value().endPoint({forward ? 1.0f : 0.0f, 0.0f});

    auto follow = compositor_.createVector3KeyFrameAnimation();
    follow.duration(kLeafSlide);
    follow.insertKeyFrame(0.0f, Vector3{edge + lead, 0.0f, 0.0f}, easing);
    follow.insertKeyFrame(1.0f, Vector3{edge + lead - direction * travel, 0.0f, 0.0f}, easing);

    // По ходу переворота тень расходится и светлеет. Растёт она от той своей
    // стороны, что прижата к бумаге: центр преобразования стоит на кромке, и
    // полоска раздаётся от неё наружу, никуда не съезжая. Градиент растянут в
    // долях полоски, так что вместе с ней растягивается и спад — половину
    // мягкости даёт уже одно это.
    fold.centerPoint({forward ? 0.0f : width_, 0.0f, 0.0f});

    // Полоска нарезана целым разворотом, а ширину тени задаёт доля от неё:
    // от нуля в начале до целой страницы в конце. Прямая через эти две точки
    // проходит серединой ровно по половине страницы — то, чем переворот и
    // меряется на глаз.
    const float span = width_ > 0.0f ? leftPage * kFoldOfPage / width_ : 0.0f;

    auto widen = compositor_.createVector3KeyFrameAnimation();
    widen.duration(kLeafSlide);
    widen.insertKeyFrame(0.0f, Vector3{0.0f, 1.0f, 1.0f}, easing);
    widen.insertKeyFrame(1.0f, Vector3{span, 1.0f, 1.0f}, easing);

    // Последним движением тень сходит на нет — и открывает нарисованный
    // средник, который всё это время лежал на самой странице. Это и есть
    // подмена: к концу переворота справа от корешка остаётся ровно то же, что
    // и слева, а движущейся тени, которой там больше нечего делать, не
    // остаётся вовсе.
    auto lighten = compositor_.createScalarKeyFrameAnimation();
    lighten.duration(kLeafSlide);
    lighten.insertKeyFrame(0.0f, kFoldDense, easing);
    lighten.insertKeyFrame(kHandover, kFoldFaint, easing);
    lighten.insertKeyFrame(1.0f, 0.0f, easing);

    // Вторую половину мягкости даёт середина спада: пока она у кромки, тень
    // читается краем, а уехав к дальнему концу — размывается целиком.
    auto flatten = compositor_.createScalarKeyFrameAnimation();
    flatten.duration(kLeafSlide);
    flatten.insertKeyFrame(0.0f, kFoldMidStop, easing);
    flatten.insertKeyFrame(1.0f, kFoldMidSoft, easing);

    // Приходящий лист выезжает из-под уходящего, держась правым краем за его
    // кромку (назад — левым за левую), и растёт от нулевой ширины до целой
    // страницы. К последнему кадру он встаёт ровно на её место, и разворот в
    // конце анимации целиком новый — ни одного чужого столбца.
    //
    // Устроен он двумя прямыми. Лист лежит целым разворотом, сдвинутым к
    // кромке, а видно его через окно, у которого внешний край страницы открыт
    // с самого начала, а сторона у корешка закрыта и открывается по ходу: лист
    // выезжает наружным краем вперёд, а не разворачивается от корешка.
    // На середине окно доходит до половины страницы ровно тогда, когда кромка
    // проходит половину своего пути, — потому старая правая страница там и
    // скрывается целиком, ни раньше ни позже.
    const float shift = forward ? width_ : -width_;
    const wchar_t* const opening = forward ? L"RightInset" : L"LeftInset";
    const float openTo = forward ? rightPage : leftPage;

    // Внешняя сторона окна стоит на краю разворота и не двигается; та, что у
    // корешка, открывает страницу от края к нему.
    comingCrop.leftInset(forward ? 0.0f : width_);
    comingCrop.rightInset(forward ? width_ : 0.0f);

    auto slide = compositor_.createVector3KeyFrameAnimation();
    slide.duration(kLeafSlide);
    slide.insertKeyFrame(0.0f, Vector3{shift, 0.0f, 0.0f}, easing);
    slide.insertKeyFrame(1.0f, Vector3{0.0f, 0.0f, 0.0f}, easing);

    auto open = compositor_.createScalarKeyFrameAnimation();
    open.duration(kLeafSlide);
    open.insertKeyFrame(0.0f, width_, easing);
    open.insertKeyFrame(1.0f, openTo, easing);

    // Тень наружного края идёт за самим краем. Край — это ведущая сторона
    // листа, и едет она через всё окно: от дальнего края разворота до
    // ближнего, то есть ровно вдвое быстрее кромки.
    const float band = width_ * kEdgeOfWindow;
    const float behind = forward ? -band : 0.0f;

    edgeBrush_.value().startPoint({forward ? 1.0f : 0.0f, 0.0f});
    edgeBrush_.value().endPoint({forward ? 0.0f : 1.0f, 0.0f});

    auto trail = compositor_.createVector3KeyFrameAnimation();
    trail.duration(kLeafSlide);
    trail.insertKeyFrame(0.0f, Vector3{edge + behind, 0.0f, 0.0f}, easing);
    trail.insertKeyFrame(1.0f, Vector3{edge - direction * width_ + behind, 0.0f, 0.0f}, easing);

    // Полутон изгиба живёт в координатах самого листа, а не полосы: лист
    // едет, и вместе с ним едет всё, что на нём нарисовано. Двигаться ему
    // остаётся ровно настолько, насколько открывается окно кроя, — тем он и
    // держится у той стороны листа, что уходит в сгиб.
    const float bendWidth = width_ * kBendOfWindow;
    const float bendFrom = forward ? -bendWidth : width_;
    const float bendTo = forward ? leftPage - bendWidth : leftPage;

    bendBrush_.value().startPoint({forward ? 1.0f : 0.0f, 0.0f});
    bendBrush_.value().endPoint({forward ? 0.0f : 1.0f, 0.0f});

    auto curve = compositor_.createVector3KeyFrameAnimation();
    curve.duration(kLeafSlide);
    curve.insertKeyFrame(0.0f, Vector3{bendFrom, 0.0f, 0.0f}, easing);
    curve.insertKeyFrame(1.0f, Vector3{bendTo, 0.0f, 0.0f}, easing);

    // Полутон изгиба подменяется тем же нарисованным средником и тем же
    // движением: к последнему кадру приходящий лист встаёт на место, и его
    // собственная половина средника оказывается ровно там, где полутон только
    // что был. Не гасить его — значит удвоить тень вдвое против левой.
    auto settle = compositor_.createScalarKeyFrameAnimation();
    settle.duration(kLeafSlide);
    settle.insertKeyFrame(0.0f, 1.0f, easing);
    settle.insertKeyFrame(kHandover, 1.0f, easing);
    settle.insertKeyFrame(1.0f, 0.0f, easing);

    fold.isVisible(true);
    rim.isVisible(true);
    coming.isVisible(true);

    // Конец здесь, в отличие от обычного переворота, отслеживается: за ним
    // может стоять очередь. Говорит о нём пакет, а не таймер: время анимации
    // отсчитывает композитор, и его ответ — единственный, который не разойдётся
    // с картинкой.
    turning_ = true;
    turnForward_ = forward;
    const std::uint32_t epoch = ++turnEpoch_;

    auto batch = compositor_.createScopedBatch(CompositionBatchTypes::Animation);

    goingCrop.startAnimation(inset, crawl);
    fold.startAnimation(L"Offset", follow);
    fold.startAnimation(L"Scale", widen);
    fold.startAnimation(L"Opacity", lighten);
    foldMid_.value().startAnimation(L"Offset", flatten);
    rim.startAnimation(L"Offset", trail);
    coming.startAnimation(L"Offset", slide);
    comingCrop.startAnimation(opening, open);
    bend_.value().startAnimation(L"Offset", curve);
    bend_.value().startAnimation(L"Opacity", settle);

    batch.add_onCompleted([this, alive = std::weak_ptr<int>(alive_), epoch](
                        Object const&, CompositionBatchCompletedEventArgs&) {
        // Полосы может уже не быть: книгу закрывают и посреди переворота, а
        // пакет о конце сообщает после него.
        if (alive.expired()) return;

        // Пакет закрывает и остановленная анимация, так что «конец» приходит
        // и на переворот, который отменили встречным листанием. Прибирать за
        // ним нечего — за него уже прибрал resetSheets следующего.
        if (epoch != turnEpoch_) return;

        turnCompleted();
    });
    batch.end();
}

void BookView::goToCharOffset(std::uint32_t charOffset) {
    if (!book_) return;

    // Прыжок по закладке, оглавлению или находке поиска — это разрыв ленты, а
    // не листание: разворот встаёт с колонки, где лежит место, без анимации
    // перехода. Досчёт нужной главы делает columnOf; фоновые порции прежней
    // главы, если ушли в другую, устаревают — их отменяет relayout при первом
    // же движении, а до того они молча пройдут по новой текущей главе.
    showColumn(columnOf(charOffset));
}

float BookView::columnLeft(std::size_t index) const {
    if (!book_) return 0.0f;

    const float margin = width_ * marginFraction_;
    const float column = book_->paginator().style().width;
    const float gutter = margin * kGutterOfMargin;

    // Первая колонка начинается ровно от поля: колонки занимают всё место
    // между полями, и центрировать тут нечего.
    return margin + static_cast<float>(index) * (column + gutter);
}

float BookView::spine() const {
    // Считается от колонок, а не как половина полосы. Поля симметричны, и
    // ответ тот же, но зависеть от этого незачем: корешок — это середина
    // средника, и сказано это должно быть про средник.
    const float gutter = width_ * marginFraction_ * kGutterOfMargin;
    return columnLeft(1) - gutter * 0.5f;
}

const fb3::Node* BookView::noteAt(Point point, Point& anchor) const {
    if (!book_) return nullptr;

    for (std::size_t column = 0; column < spread_.size(); ++column) {
        const float left = columnLeft(column);

        for (const typography::PlacedLine& placed : spread_[column]->lines) {
            const typography::Line& line = *placed.line;
            const float baseline = kVerticalMargin + placed.baseline;

            // По вертикали засчитываем всю строку, а не только надстрочный
            // знак: попасть мышью в шесть пикселей высотой нельзя.
            if (point.y < baseline - line.ascent || point.y > baseline + line.descent) continue;

            // Зона щелчка шире самого знака на треть кегля с каждой стороны —
            // по той же причине.
            const float slack = fontSize_ * 0.33f;

            for (const typography::PlacedNote& note : line.notes) {
                const float x = left + placed.x + note.x;
                if (point.x < x - slack || point.x > x + note.width + slack) continue;

                anchor = {x + note.width * 0.5f, baseline + line.descent};
                return note.target;
            }
        }
    }

    return nullptr;
}

void BookView::updateBackdrop() {
    const std::filesystem::path wanted = backdropFile();
    if (wanted.empty()) {
        backdropSource_.Reset();
        photoBitmap_.Reset();
        backdropLoaded_.clear();
        return;
    }

    // Раскодированный снимок держится, пока путь тот же: decodeImage читает диск
    // и дорог. Смена снимка сбрасывает и кэш битмапа устройства — его заведёт
    // заново drawThemeBackdrop, вкомпоновывая фото прямо в поверхность страницы.
    if (backdropLoaded_ != wanted.native()) {
        backdropSource_ = decodeImage(wanted);
        backdropLoaded_ = wanted.native();
        photoBitmap_.Reset();
    }
}

bool BookView::ensureWarp(ID2D1DeviceContext* context) {
    const D2D1_SIZE_U pixels{static_cast<UINT32>(width_ * scale_ + 0.5f),
                             static_cast<UINT32>(height_ * scale_ + 0.5f) * 2};
    if (pixels.width == 0 || pixels.height == 0) return false;

    if (!warpLayer_ || warpPixels_.width != pixels.width || warpPixels_.height != pixels.height) {
        if (!warpContext_) {
            Microsoft::WRL::ComPtr<ID2D1Device> device;
            context->GetDevice(&device);
            if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                              &warpContext_)))
                return false;
            // Те же режимы, что redraw() ставит поверхности: слой — та же
            // страница, только в другой битмап.
            warpContext_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            warpContext_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        warpLayer_.Reset();
        warpMap_.Reset();
        warpFlat_ = false;

        const D2D1_BITMAP_PROPERTIES1 layerProps{
            {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
            96.0f, 96.0f, D2D1_BITMAP_OPTIONS_TARGET, nullptr};
        if (FAILED(warpContext_->CreateBitmap(pixels, nullptr, 0, &layerProps, &warpLayer_)))
            return false;
        warpPixels_ = pixels;
    }

    // Эффекты — раньше проверки плоскости: плоскому предпросмотру под
    // конфигуратором ужатие нужно тоже — его слой идёт мимо смещения, но
    // через ту же вертикальную двойку. Подключение входов здесь не делается:
    // кто за кем стоит в цепочке, каждый кадр решает drawPage().
    if (!warpDisplace_) {
        warpContext_->CreateEffect(CLSID_D2D1DisplacementMap, &warpDisplace_);
        warpContext_->CreateEffect(CLSID_D2D1Scale, &warpShrink_);
        if (!warpDisplace_ || !warpShrink_) {
            warpDisplace_.Reset();
            warpShrink_.Reset();
            return false;
        }
        warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT,
                                D2D1_CHANNEL_SELECTOR_R);
        warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT,
                                D2D1_CHANNEL_SELECTOR_G);
        warpShrink_->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(1.0f, 0.5f));
        warpShrink_->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE,
                              D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }

    // Обложка с нетронутыми точками — плоская: гнуть нечего, полоса рисуется
    // напрямую. Ответ запомнен, чтобы не пересчитывать кривые на каждый кадр;
    // сбрасывают его смена темы, реестра обложек и размера полосы.
    if (warpFlat_) return false;

    if (!warpMap_) {
        // Отклонения краёв от их прямых начальных линий, в долях высоты
        // полосы, по значению на столбец карты. Встроенная тема — идеализация:
        // купол синуса, вверх у верхнего края и вниз у нижнего. У обложки
        // вместо синуса — четыре кривые, снятые мастером с самого снимка:
        // у каждой границы каждого листа изгиб свой, и середина разворота —
        // граница между левой парой и правой.
        const int columns = static_cast<int>(pixels.width);
        std::vector<float> topEdge;
        std::vector<float> bottomEdge;

        if (const Skin* skin = preview_ ? &*preview_ : activeSkin()) {
            topEdge.resize(static_cast<std::size_t>(columns));
            bottomEdge.resize(static_cast<std::size_t>(columns));
            for (int x = 0; x < columns; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(columns);
                const bool left = u < 0.5f;
                topEdge[static_cast<std::size_t>(x)] =
                    edgeAt(left ? skin->topLeft : skin->topRight, u) - kEdgeInset;
                bottomEdge[static_cast<std::size_t>(x)] =
                    edgeAt(left ? skin->bottomLeft : skin->bottomRight, u) -
                    (1.0f - kEdgeInset);
            }
        } else {
            topEdge.resize(static_cast<std::size_t>(columns));
            bottomEdge.resize(static_cast<std::size_t>(columns));
            const float amplitude = kBulge / height_;
            for (int x = 0; x < columns; ++x) {
                const float across = (static_cast<float>(x) + 0.5f) / static_cast<float>(columns);
                const float dome =
                    std::sin(std::numbers::pi_v<float> * std::abs(across - 0.5f) * 2.0f);
                topEdge[static_cast<std::size_t>(x)] = -dome * amplitude;
                bottomEdge[static_cast<std::size_t>(x)] = dome * amplitude;
            }
        }

        float amplitude = 0.0f;
        for (int x = 0; x < columns; ++x) {
            amplitude = std::max({amplitude, std::abs(topEdge[static_cast<std::size_t>(x)]),
                                  std::abs(bottomEdge[static_cast<std::size_t>(x)])});
        }
        if (amplitude <= 0.0f) {
            warpFlat_ = true;
            return false;
        }
        warpAmplitude_ = amplitude;

        // Карта хранит чистую форму изгиба: канал G — доля смещения по Y от
        // размаха эффекта, 128 — «не смещать». Смещение обратное (эффект
        // читает «откуда взять», а не «куда сдвинуть»), поэтому у строк,
        // уезжающих вверх, в карте стоит плюс — взять снизу. Между краями
        // отклонение интерполируется линейно по высоте.
        std::vector<std::uint8_t> bytes(std::size_t{pixels.width} * pixels.height * 4);
        for (UINT32 y = 0; y < pixels.height; ++y) {
            const float down = (static_cast<float>(y) + 0.5f) / static_cast<float>(pixels.height);
            std::uint8_t* row = &bytes[std::size_t{y} * pixels.width * 4];
            for (UINT32 x = 0; x < pixels.width; ++x) {
                const float shift = topEdge[x] + (bottomEdge[x] - topEdge[x]) * down;
                std::uint8_t* px = row + std::size_t{x} * 4;
                px[0] = 128;   // B — не читается
                px[1] = static_cast<std::uint8_t>(127.5f * (1.0f - shift / amplitude) + 0.5f);
                px[2] = 128;   // R — канал X, нейтрально
                px[3] = 255;
            }
        }
        const D2D1_BITMAP_PROPERTIES1 mapProps{
            {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
            96.0f, 96.0f, D2D1_BITMAP_OPTIONS_NONE, nullptr};
        if (FAILED(warpContext_->CreateBitmap(pixels, bytes.data(), pixels.width * 4, &mapProps,
                                              &warpMap_)))
            return false;
    }

    warpDisplace_->SetInput(0, warpLayer_.Get());
    warpDisplace_->SetInput(1, warpMap_.Get());
    // Размах — в пикселях слоя: наибольшее отклонение краёв в долях высоты —
    // это warpAmplitude_·height_·scale_ пикселей поверхности и вдвое больше в
    // слое двойной высоты; ещё двойка — потому что карта отклоняется от
    // середины не дальше половины размаха.
    warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE,
                            4.0f * warpAmplitude_ * height_ * scale_);
    return true;
}

bool BookView::ensureBlur() {
    if (warpBlur_) return true;
    if (!warpContext_) return false;

    warpContext_->CreateEffect(CLSID_D2D1GaussianBlur, &warpBlur_);
    if (!warpBlur_) return false;

    // HARD вместо мягкой кромки по умолчанию: у границ полосы размытие не
    // должно подмешивать прозрачность из-за края слоя.
    warpBlur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    return true;
}

void BookView::drawThemeBackdrop(ID2D1DeviceContext* context, float width, float height) {
    if (!backdropSource_) return;

    // Битмап устройства заводится раз на снимок и держится: CreateBitmapFromWicBitmap
    // на каждый кадр стоил бы дорого, а перерисовка бывает лишь на листании и
    // растяжке. Устройство поверхности стабильно, пока не потеряно.
    if (!photoBitmap_) {
        if (FAILED(context->CreateBitmapFromWicBitmap(backdropSource_.Get(), nullptr, &photoBitmap_)))
            return;
    }

    // На всю полосу, без сохранения пропорций (как прежняя кисть Fill): снимок
    // скомпонован под полосу — книга по центру, стол по краям.
    context->DrawBitmap(photoBitmap_.Get(), D2D1::RectF(0.0f, 0.0f, width, height), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void BookView::drawPage(ID2D1DeviceContext* context, float width, float height) {
    // Ровная тема кроет лист бумагой; тема с подложкой начинает лист с самой
    // фотоподложки, а текст ложится поверх неё — всё в одну поверхность, одним
    // битмапом, без отдельного визуала под страницей.
    if (backdropFile().empty()) {
        context->Clear(paper().background);
        drawPageContent(context, width, height);
        return;
    }
    // Фото-тема: фотоподложка — в ту же поверхность, под текстом, чтобы страница
    // осталась одним битмапом. Не раскодировалась — бумага темы, чтобы задник не
    // сквозил.
    if (backdropSource_) {
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        drawThemeBackdrop(context, width, height);
    } else {
        context->Clear(paper().background);
    }

    const bool warped = ensureWarp(context);

    // Под конфигуратором изгиба набор слегка размывается: текст отступает на
    // второй план, и направляющие мастера читаются чётче. Ещё не согнутая —
    // плоская — обложка идёт тем же слоем, но мимо смещения: размытию нужен
    // образ страницы, а прямому рисунку в поверхность его не дать.
    const bool blurred = preview_ && (warped || warpFlat_) && ensureBlur();

    if (!warped && !blurred) {
        drawPageContent(context, width, height);
        return;
    }

    warpContext_->SetTarget(warpLayer_.Get());
    warpContext_->BeginDraw();
    warpContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    // Вертикальная двойка растит слой, горизонтальный масштаб — тот же, что
    // redraw() даёт поверхности: содержимое рисуется в DIP, слой — в пикселях.
    warpContext_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_ * 2.0f));
    drawPageContent(warpContext_.Get(), width, height);
    if (FAILED(warpContext_->EndDraw())) return;   // фон с фотографией уже есть

    // Хвост цепочки собирается по месту: изгиб, если есть что гнуть, потом
    // ужатие двойной высоты, потом размытие, если открыт конфигуратор.
    if (warped) {
        warpShrink_->SetInputEffect(0, warpDisplace_.Get());
    } else {
        warpShrink_->SetInput(0, warpLayer_.Get());
    }

    ID2D1Effect* tail = warpShrink_.Get();
    if (blurred) {
        warpBlur_->SetInputEffect(0, tail);
        warpBlur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kPreviewBlur * scale_);
        tail = warpBlur_.Get();
    }

    // Выход эффектов — в пикселях поверхности, поэтому масштаб DIP→пиксели из
    // трансформа на время вынимается: остаётся только смещение атласа.
    D2D1_MATRIX_3X2_F outer{};
    context->GetTransform(&outer);
    context->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / scale_, 1.0f / scale_) *
                          *D2D1::Matrix3x2F::ReinterpretBaseType(&outer));
    Microsoft::WRL::ComPtr<ID2D1Image> page;
    tail->GetOutput(&page);
    context->DrawImage(page.Get());
    context->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&outer));
}

void BookView::drawPageContent(ID2D1DeviceContext* context, float width, float height) {
    const Theme& shade = paper();
    const float margin = width_ * marginFraction_;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dimBrush;
    context->CreateSolidColorBrush(shade.text, &textBrush);
    context->CreateSolidColorBrush(shade.dim, &dimBrush);
    if (!textBrush || !dimBrush) return;

    // Колонки разворота — готовая лента (buildSpread): каждая своя страница,
    // идущие подряд, при нужде со стыка из следующей главы. Разворот стоит по
    // середине окна, остаток ширины уходит в поля поровну.
    for (std::size_t column = 0; column < spread_.size(); ++column) {
        const float left = columnLeft(column);
        const typography::Page& page = *spread_[column];

        for (const typography::PlacedImage& image : page.images) {
            if (ID2D1Bitmap1* bitmap = book_->bitmap(image.imageIndex, context)) {
                const D2D1_RECT_F target =
                    D2D1::RectF(left + image.x, kVerticalMargin + image.y,
                                left + image.x + image.width,
                                kVerticalMargin + image.y + image.height);
                context->DrawBitmap(bitmap, target, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }

        for (const typography::PlacedLine& placed : page.lines) {
            typography::drawLine(context, *placed.line, left + placed.x,
                                 kVerticalMargin + placed.baseline, textBrush.Get());
        }
    }

    // Средник — то, чем две колонки на экране становятся раскрытой книгой: у
    // корешка бумага уходит вглубь, и туда не достаёт свет. Рисуется вместе со
    // страницей, а не отдельным визуалом, и это главное в нём: средник
    // принадлежит развороту, а не перевороту. Поэтому он есть всегда — и до
    // первого листания, и после перевёрстки, — и едет вместе со всем, что этот
    // разворот показывает: и с уходящим листом, и с приходящим. Движущиеся
    // тени переворота в конце его же и подменяют.
    //
    // Поверх текста, а не под ним: тень у корешка ложится на страницу целиком,
    // вместе с набором.
    if (columns_ == 2) {
        const float centre = spine();
        const float band = width * kBendOfWindow;
        const D2D1_COLOR_F& hue = paper().shadow;
        const D2D1_COLOR_F clear = tintedF(hue, 0.0f);
        const D2D1_COLOR_F dense = tintedF(hue, alphaOf(kBendNear));
        const D2D1_COLOR_F mid = tintedF(hue, alphaOf(kBendMid));

        // Тень зеркальна относительно корешка: тот же спад, что у полутона
        // изгиба, но в обе стороны — потому в конце переворота обе половины и
        // сходятся с тем, что было нарисовано.
        const D2D1_GRADIENT_STOP ramp[] = {
            {0.0f, clear},
            {0.5f - kBendMidStop * 0.5f, mid},
            {0.5f, dense},
            {0.5f + kBendMidStop * 0.5f, mid},
            {1.0f, clear},
        };

        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
        context->CreateGradientStopCollection(ramp, static_cast<UINT32>(std::size(ramp)), &stops);
        if (stops) {
            Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gutter;
            context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(centre - band, 0.0f),
                                                    D2D1::Point2F(centre + band, 0.0f)),
                stops.Get(), &gutter);
            if (gutter) {
                context->FillRectangle(D2D1::RectF(centre - band, 0.0f, centre + band, height),
                                       gutter.Get());
            }
        }
    }

    if (statusFormat_) {
        // На развороте колонцифра называет обе страницы: читатель видит две, и
        // одна в счётчике расходилась бы с тем, что перед глазами.
        //
        // Номер левой колонки известен сразу — видимый разворот посчитан
        // начисто в тот же кадр. Общее же число страниц главы становится
        // известно, только когда её набор кончился; пока нет — многоточие:
        // число, которое сейчас сменится другим, хуже честного молчания.
        // Процент при этом верен всегда, потому что считается по символам
        // книги, а место чтения перевёрстка почти не двигает.
        // Номера — по левой главе: разворот на стыке кончается колонками
        // следующей, но «стр. X из M» называет ту главу, где стоит читатель, и
        // не залезает в номера соседней. Сколько колонок разворота её —
        // spreadOwnColumns_.
        const std::size_t first = page_;
        const std::size_t own = std::max<std::size_t>(spreadOwnColumns_, 1);
        const std::wstring numbers =
            own <= 1 ? std::format(L"{}", first + 1)
                     : std::format(L"{}–{}", first + 1, first + own);
        const std::wstring total = book_->paginator().isComplete()
                                       ? std::format(L"{}", std::max<std::size_t>(pageCount(), 1))
                                       : std::wstring{L"…"};

        // Номер и общее число — по главе: пагинатор знает лишь её, и «из M»
        // здесь значит «из стольких страниц в этой главе». Процент — по всей
        // книге, по символам; им читатель и меряет весь путь.
        const std::wstring status =
            std::format(L"Глава {} · стр. {} из {}     {:.0f}%",
                        book_->currentChapter() + 1, numbers, total, progress() * 100.0f);
        context->DrawText(status.c_str(), static_cast<UINT32>(status.size()), statusFormat_.Get(),
                          D2D1::RectF(margin, height - kVerticalMargin, width - margin, height),
                          dimBrush.Get());
    }
}

void BookView::drawInvitation(ID2D1DeviceContext* context, float width, float height) {
    const Theme& shade = paper();
    context->Clear(backdropFile().empty() ? shade.background
                                          : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    IDWriteFactory* const dwrite = dwriteFactory();
    if (!dwrite) return;

    if (!invitationFormat_) {
        dwrite->CreateTextFormat(L"Georgia", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STRETCH_NORMAL, 22.0f,
                                 L"ru-RU", &invitationFormat_);
        if (!invitationFormat_) return;
        invitationFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        invitationFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(shade.dim, &brush);
    if (!brush) return;

    static constexpr wchar_t kInvitation[] = L"Перетащите книгу FB3 в это окно";
    context->DrawText(kInvitation, static_cast<UINT32>(sizeof(kInvitation) / sizeof(wchar_t) - 1),
                      invitationFormat_.Get(), D2D1::RectF(0, 0, width, height), brush.Get());
}

}  // namespace bukvitsa::reader
