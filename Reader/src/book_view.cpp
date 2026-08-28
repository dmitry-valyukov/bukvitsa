#include <algorithm>
#include <filesystem>
#include <format>

#include <dwrite.h>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "book_view.h"

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

/// Средник — расстояние между колонками, в долях поля. Больше поля: край
/// полосы у окна глаз видит и так, а вот две колонки, разделённые тем же
/// зазором, что и край, сливаются в одну, и на возврате глаз перескакивает
/// в соседнюю.
constexpr float kGutterOfMargin = 1.75f;

/// Отступы сверху и снизу, в DIP. Регулировка «Поля» правит только
/// горизонтальные поля: ими читатель выбирает ширину строки, а высоте полосы
/// выбирать нечего — она и так вся, что осталось от окна.
constexpr float kVerticalMargin = 50.0f;

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

/// Раскодирует фотографию-подложку темы. Путь в теме — от исполняемого файла,
/// как и у остальных ресурсов из Assets, поэтому он достраивается от модуля,
/// а не от текущего каталога: тот зависит от того, откуда читалку запустили.
/// Фабрика WIC своя и на один вызов: подложка загружается при смене темы, а
/// не в цикле, и держать фабрику ради этого незачем.
Microsoft::WRL::ComPtr<IWICFormatConverter> decodeBackdrop(const wchar_t* relative) {
    using Microsoft::WRL::ComPtr;

    wchar_t module[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) return nullptr;
    const std::filesystem::path path = std::filesystem::path(module).parent_path() / relative;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad, &decoder)))
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
        return nullptr;

    return converter;
}

}  // namespace

BookView::BookView(const Compositor& compositor, const DispatcherQueue& queue)
    : compositor_(compositor), queue_(queue), note_(compositor) {
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

    // Страница живёт в отдельном узле, а не прямо в корне: поверхность
    // прицеплена к нему дочерним визуалом, а всё, что кладут поверх полосы
    // (сноска, панель), — это дети корня, идущие после него. Порядок детей и
    // есть порядок по глубине.
    pageHost_ = Grid{};

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

    ElementCompositionPreview::setElementChildVisual(pageHost_.value(), sheets_.value());

    auto tree = Grid{
        // Корень берёт фокус на себя: событие клавиши начинается у того, на
        // чём фокус, и пока фокуса нет ни на чём, ловить нечего.
        isTabStop = true,

        // Кисть здесь нужна по двум причинам, и обе неочевидны.
        //
        // Панель без кисти в проверке попадания не участвует вовсе, и щелчок
        // по полосе не доходил никуда: ни до знака сноски, ни до трети
        // страницы. И кисть эта — цвета бумаги, а не прозрачная: страницу
        // рисует композитор поверх неё, но в те кадры, когда он ещё не
        // нарисовал (первый показ, растянутое мышью окно), из-под неё должна
        // проглядывать бумага, а не белизна окна.
        background = SolidColorBrush{ARGB{argbOf(kThemes[0].background)}},

        pageHost_.value(),
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
                if (book_) goTo(catchUpTo(0));
                break;
            case VirtualKey::End:
                // Конец книги известен только досчитанной, поэтому здесь
                // чистовой набор доводится до самого конца.
                if (book_) goTo(catchUpTo(book_->characterCount()));
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
    return book_->paginator().blocks();
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

void BookView::setTheme(int index) {
    theme_ = ((index % kThemeCount) + kThemeCount) % kThemeCount;
    note_.hide();   // подложка всплывашки покрашена прошлой темой
    root_.value().background(SolidColorBrush{ARGB{argbOf(paper().background)}});
    applyShadowTint();   // тени тоже покрашены прошлой темой
    redraw();
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

void BookView::setMargin(float ems) {
    const float wanted = std::clamp(ems, 0.5f, 10.0f);
    if (wanted == marginEms_) return;
    marginEms_ = wanted;
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
    const float margin = fontSize_ * marginEms_;
    const float available = width_ - margin * 2.0f;
    const float gutters = margin * kGutterOfMargin * static_cast<float>(columns - 1);
    return (available - gutters) / static_cast<float>(columns) / characterWidth();
}

int BookView::chooseColumns(bool sticky) const {
    const float margin = fontSize_ * marginEms_;
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
        draft_ = false;
        redraw();
        return;
    }

    const float margin = fontSize_ * marginEms_;
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

    // Вот ради чего позиции чтения хранятся в символах: полоса стала другой, а
    // читатель остался на том же месте — и видит его сейчас же. Грязная
    // страница начинается ровно с той буквы, на которой он стоял, и считается
    // за единицы миллисекунд, потому что считает только то, что видно.
    //
    // Место чтения при этом не двигается ни на символ. Раньше оно прижималось
    // к началу свежей страницы, и от каждой смены кегля прогресс чуть уезжал;
    // теперь прижимать не к чему — страница начинается с него самого.
    //
    book_->paginator().draftAt(pageStyle_, readingPosition_,
                               static_cast<std::size_t>(columns_));
    draft_ = true;
    numberKnown_ = false;
    redraw();

    // А книга набирается начисто следом, порциями и в свободное время потока.
    startPagination();
}

void BookView::startPagination() {
    // Заказ уже в очереди — второй ничего не прибавит: задание всё равно
    // возьмёт ту полосу, какую застанет, а полоса к тому времени будет
    // нынешней.
    if (cleanPosted_) return;
    cleanPosted_ = true;

    std::weak_ptr<int> alive = alive_;

    // Низкий приоритет — это и есть «в свободное время»: поток сперва разберёт
    // ввод и покажет нарисованное, а уже потом возьмётся за книгу.
    queue_.tryEnqueue(DispatcherQueuePriority::Low, [this, alive] {
        if (alive.expired()) return;
        cleanPosted_ = false;
        if (!book_) return;

        const std::uint32_t epoch = ++paginationEpoch_;
        book_->paginator().beginLayout(pageStyle_);
        paginateChunk(epoch);
    });
}

void BookView::paginateChunk(std::uint32_t epoch) {
    if (epoch != paginationEpoch_ || !book_) return;

    const bool more = book_->paginator().advance(kPaginationSlice);

    // Чистовой набор не подменяет собой то, что читатель видит. Он делит
    // полосу иначе — с начала книги, а не с места чтения, — и подмена была бы
    // прыжком текста под глазами. Меняется только колонцифра: у показанного
    // разворота появляется номер, а у книги — общее число страниц.
    const bool known = cleanSpread().has_value();
    bool changed = known != numberKnown_;
    numberKnown_ = known;

    if (!more) changed = true;
    if (changed) redraw();
    if (!more) return;

    std::weak_ptr<int> alive = alive_;
    queue_.tryEnqueue(DispatcherQueuePriority::Low, [this, alive, epoch] {
        if (alive.expired()) return;
        paginateChunk(epoch);
    });
}

std::size_t BookView::catchUpTo(std::uint32_t charOffset) {
    typography::Paginator& paginator = book_->paginator();

    // Энергично, без срока: порции хороши, пока читатель читает, а он ждёт
    // ответа. Остаток книги при этом по-прежнему добирается порциями — та,
    // что уже стоит в очереди, просто продолжит с того, на чём мы кончили.
    paginator.advanceTo(charOffset);
    if (paginator.pageCount() == 0) return 0;

    std::size_t at = paginator.pageForCharOffset(charOffset);
    at -= at % static_cast<std::size_t>(columns_);

    // Разворот — это несколько страниц, и довести набор до первой из них мало.
    paginator.advanceToPage(at + static_cast<std::size_t>(columns_));
    return at;
}

std::optional<std::size_t> BookView::cleanSpread() const {
    if (!book_) return std::nullopt;
    if (!draft_) return page_;

    const typography::Paginator& paginator = book_->paginator();
    if (paginator.pageCount() == 0) return std::nullopt;

    // Номер известен, только когда набор ушёл за эту страницу: пока она
    // последняя, на ней ещё будет место, и номер следующей ещё не решён.
    if (!paginator.isComplete() &&
        paginator.page(paginator.pageCount() - 1).firstCharOffset <= readingPosition_)
        return std::nullopt;

    const std::size_t at = paginator.pageForCharOffset(readingPosition_);
    return at - at % static_cast<std::size_t>(columns_);
}

void BookView::turnDraftForward() {
    // Очередь та же, что и у чистового листания: читатель, нажавший «дальше»
    // дважды, просил два разворота, а не один.
    if (turning_ && turnForward_) {
        ++pending_;
        return;
    }

    cancelTurn();
    startDraftTurn();
}

void BookView::startDraftTurn() {
    const auto columns = static_cast<std::size_t>(columns_);
    typography::Paginator& paginator = book_->paginator();

    // Следующая страница нужна ровно здесь: её первый символ — то место, с
    // которого начинается новый разворот. Считать её заранее, на каждое
    // движение мыши, значило бы считать зря, поэтому грязная вёрстка
    // досчитывается по надобности — и говорит, если досчитывать уже нечего.
    if (!paginator.draftUpTo(columns + 1)) {
        pending_ = 0;   // книга кончилась
        return;
    }

    note_.hide();   // страница ушла, а сноска на ней осталась бы висеть
    readingPosition_ = paginator.draftPage(columns).firstCharOffset;
    paginator.draftAt(pageStyle_, readingPosition_, columns);

    // Порядок тот же, что и у чистового листания: к началу анимации верная
    // страница уже нарисована и уже лежит внизу.
    resting_ = 1 - resting_;
    redraw();

    if (columns_ == 2) {
        animateSpreadTurn(true);
    } else {
        animateTurn(true);
    }

    if (onPositionChanged) onPositionChanged(readingPosition_);
}

const typography::Page* BookView::spreadPage(std::size_t column) const {
    if (!book_) return nullptr;

    const typography::Paginator& paginator = book_->paginator();
    if (draft_) {
        if (column >= paginator.draftCount()) return nullptr;
        return &paginator.draftPage(column);
    }

    const std::size_t number = page_ + column;
    if (number >= paginator.pageCount()) return nullptr;
    return &paginator.page(number);
}

void BookView::redraw() {
    if (surface_.empty() || width_ <= 0.0f || height_ <= 0.0f) return;

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

        if (spreadPage(0)) {
            drawPage(context, width_, height_);
        } else {
            drawInvitation(context, width_, height_);
        }
    });
}

void BookView::turnPage(int delta) {
    if (!book_) return;

    if (draft_) {
        // Вперёд грязная вёрстка листается сама: следующий разворот считается
        // тем же способом, что и нынешний, и книга при этом ни при чём.
        if (delta > 0) {
            turnDraftForward();
            return;
        }

        // А назад — нет. Начало предыдущей страницы известно только тому, кто
        // набрал книгу с начала, поэтому листание назад и есть тот случай,
        // ради которого чистовой набор всё это время считался.
        const auto columns = static_cast<std::size_t>(columns_);
        const std::size_t at = catchUpTo(readingPosition_);
        goTo(at >= columns ? at - columns : 0);
        return;
    }

    if (pageCount() == 0) return;

    // Листается разворот целиком: на две колонки читатель за раз прочитывает
    // две страницы, и перелистывать по одной значило бы половину показывать
    // дважды.
    //
    // Считается от конца очереди, а не от видимого разворота. Пока переворот
    // идёт, читатель уже попросил следующую страницу, и второе нажатие обязано
    // прибавиться к первому, а не повторить его.
    const auto count = static_cast<std::ptrdiff_t>(pageCount());
    const auto step = static_cast<std::ptrdiff_t>(delta) * columns_;
    const auto target = std::clamp(static_cast<std::ptrdiff_t>(queueEnd()) + step,
                                   static_cast<std::ptrdiff_t>(0), count - 1);
    goTo(static_cast<std::size_t>(target));
}

std::size_t BookView::queueEnd() const {
    if (pending_ == 0) return page_;

    // Шаги очереди — соседние развороты в одну сторону, и в очередь попадает
    // только тот, что в книге есть (см. goTo): значит, конец считается
    // умножением и за край не выходит.
    const auto step = static_cast<std::ptrdiff_t>(columns_) * pending_;
    const auto tail = static_cast<std::ptrdiff_t>(page_) + (turnForward_ ? step : -step);
    return static_cast<std::size_t>(tail);
}

std::size_t BookView::neighbourSpread(std::size_t page, bool forward) const {
    const auto count = static_cast<std::ptrdiff_t>(pageCount());
    const auto step = static_cast<std::ptrdiff_t>(columns_) * (forward ? 1 : -1);
    const auto next = std::clamp(static_cast<std::ptrdiff_t>(page) + step,
                                 static_cast<std::ptrdiff_t>(0), count - 1);

    // Упор в край книги отдаёт тот же разворот, с которого шли: у goTo это и
    // значит «идти некуда».
    auto spread = static_cast<std::size_t>(next);
    return spread - spread % static_cast<std::size_t>(columns_);
}

void BookView::goTo(std::size_t page) {
    if (!book_ || pageCount() == 0) return;

    // Сход с грязной вёрстки. Полоса встаёт на тот чистовой разворот, внутри
    // которого лежит место чтения, и уже от него листает дальше: у грязной
    // страницы номера нет, и мерить шаг не от чего. Зовущий к этому времени
    // уже досчитал набор до нужного места — `catchUpTo`.
    const bool leftDraft = draft_;
    if (draft_) {
        // Очередь листания вместе с ней и кончается: её шаги считались по
        // грязной вёрстке, а полоса уходит на чистовую, и вести туда, куда
        // читатель уже не собирается, незачем.
        cancelTurn();

        draft_ = false;
        page_ = book_->paginator().pageForCharOffset(readingPosition_);
        page_ -= page_ % static_cast<std::size_t>(columns_);
    }

    // Номер страницы всегда указывает на начало разворота: с него начинается
    // и отрисовка, и следующий шаг листания.
    std::size_t wanted = std::min(page, pageCount() - 1);
    wanted -= wanted % static_cast<std::size_t>(columns_);

    // Всё меряется от конца очереди: пока идёт переворот, книга считается
    // стоящей там, куда очередь придёт, а не там, где она видна.
    const std::size_t tail = queueEnd();
    if (wanted == tail && !surface_.empty()) {
        // Идти некуда — но если полоса только что сошла с грязной вёрстки,
        // показать чистовую всё равно надо: страница на экране начиналась с
        // места чтения, а чистовая начинается со своего.
        if (leftDraft) {
            readingPosition_ = book_->paginator().page(page_).firstCharOffset;
            note_.hide();
            redraw();
            if (onPositionChanged) onPositionChanged(readingPosition_);
        }
        return;   // уже здесь или уже туда идём
    }

    const bool forward = wanted > tail;

    // Ровно следующий разворот в ту же сторону встаёт в очередь и ждёт своего
    // переворота. Всё остальное — встречное листание, прыжок по закладке, по
    // оглавлению, по находке поиска — идущий переворот отменяет: очередь эта
    // ведёт туда, куда читатель уже не собирается.
    if (turning_ && forward == turnForward_ && wanted == neighbourSpread(tail, forward)) {
        ++pending_;
        return;
    }

    cancelTurn();

    if (!surface_.empty() && width_ > 0.0f) {
        startTurn(wanted, forward);
        return;
    }

    // Полосы ещё нет — ни поверхности, ни размера: страница просто ставится,
    // переворачивать нечего и нечем.
    note_.hide();
    page_ = wanted;
    readingPosition_ = book_->paginator().page(page_).firstCharOffset;
    redraw();
    if (onPositionChanged) onPositionChanged(readingPosition_);
}

void BookView::startTurn(std::size_t wanted, bool forward) {
    note_.hide();   // страница ушла, а сноска на ней осталась бы висеть

    page_ = wanted;
    readingPosition_ = book_->paginator().page(page_).firstCharOffset;

    // Новая страница рисуется на свободный лист, и он становится тем, на
    // котором книга стоит. Порядок именно такой: к началу анимации верная
    // страница уже нарисована и уже лежит внизу, поэтому сбой анимации
    // может стоить кадра, но не страницы.
    //
    // Отсюда же и причина, по которой очередь не может забегать вперёд:
    // свободный лист один, и пока по нему едет кромка, рисовать на нём
    // следующий разворот некуда. Очередь потому и хранится числом шагов, а не
    // готовыми страницами.
    resting_ = 1 - resting_;
    redraw();

    // Книжное листание — только для разворота: снимать бумагу с корешка
    // можно там, где корешок есть. В одну колонку и в три листается тем
    // же, чем листалось всегда.
    if (columns_ == 2) {
        animateSpreadTurn(forward);
    } else {
        animateTurn(forward);
    }

    if (onPositionChanged) onPositionChanged(readingPosition_);
}

void BookView::turnCompleted() {
    turning_ = false;
    if (pending_ == 0) return;

    --pending_;
    if (draft_) {
        startDraftTurn();
        return;
    }

    startTurn(neighbourSpread(page_, turnForward_), turnForward_);
}

void BookView::cancelTurn() {
    // Номер меняется — и обработчик конца, если пакет отменённого переворота
    // всё-таки о нём скажет, узнает свой номер чужим и промолчит.
    ++turnEpoch_;
    turning_ = false;
    pending_ = 0;
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
    // тем же порядком в goTo, что и у обычного переворота.
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

    // Прыжок по закладке, оглавлению или находке поиска — это место, которого
    // у грязной вёрстки нет: она умеет идти только вперёд от того, что
    // показывает. Значит, чистовой набор досчитывается до него — и полоса
    // переходит на него.
    //
    // Через номер страницы, а не прямо: место чтения обязано совпасть с
    // началом показанной страницы, иначе прогресс и закладка разойдутся с тем,
    // что видит читатель.
    goTo(catchUpTo(charOffset));
}

float BookView::columnLeft(std::size_t index) const {
    if (!book_) return 0.0f;

    const float margin = fontSize_ * marginEms_;
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
    const float gutter = fontSize_ * marginEms_ * kGutterOfMargin;
    return columnLeft(1) - gutter * 0.5f;
}

const fb3::Node* BookView::noteAt(Point point, Point& anchor) const {
    if (!book_) return nullptr;

    for (std::size_t column = 0; column < static_cast<std::size_t>(columns_); ++column) {
        const typography::Page* const shown = spreadPage(column);
        if (!shown) break;

        const float left = columnLeft(column);

        for (const typography::PlacedLine& placed : shown->lines) {
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

void BookView::drawBackdrop(ID2D1DeviceContext* context, float width, float height) {
    const wchar_t* const wanted = paper().backdrop;
    if (!wanted) return;

    if (backdropLoaded_ != wanted) {
        backdropSource_ = decodeBackdrop(wanted);
        backdropBitmap_.Reset();
        backdropLoaded_ = wanted;
    }

    if (!backdropBitmap_ && backdropSource_) {
        if (FAILED(context->CreateBitmapFromWicBitmap(backdropSource_.Get(), nullptr,
                                                      &backdropBitmap_)))
            backdropSource_.Reset();   // не вышло — больше не пытаемся
    }

    if (!backdropBitmap_) return;

    // На всю полосу, без сохранения пропорций: снимок скомпонован под полосу —
    // книга в середине, стол по краям, — и кадрирование ради пропорций резало
    // бы именно книгу. Растяжение бумажной фактуры глаз не ловит.
    context->DrawBitmap(backdropBitmap_.Get(), D2D1::RectF(0.0f, 0.0f, width, height), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void BookView::drawPage(ID2D1DeviceContext* context, float width, float height) {
    const Theme& shade = paper();
    const float margin = fontSize_ * marginEms_;

    context->Clear(shade.background);
    drawBackdrop(context, width, height);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dimBrush;
    context->CreateSolidColorBrush(shade.text, &textBrush);
    context->CreateSolidColorBrush(shade.dim, &dimBrush);
    if (!textBrush || !dimBrush) return;

    // Колонок столько, сколько поместилось по мере строки; каждая — своя
    // страница пагинатора, идущие подряд. Разворот стоит по середине окна,
    // остаток ширины уходит в поля поровну.
    std::size_t drawn = 0;

    for (std::size_t column = 0; column < static_cast<std::size_t>(columns_); ++column) {
        const typography::Page* const shown = spreadPage(column);
        if (!shown) break;
        drawn = column + 1;

        const float left = columnLeft(column);
        const typography::Page& page = *shown;

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
        // Номер известен, только когда чистовой набор ушёл за эту страницу, а
        // общее число — только когда он кончился. Пока нет — многоточие:
        // число, которое сейчас сменится другим, хуже честного молчания.
        // Процент при этом верен всегда, потому что считается по символам
        // книги, а место чтения перевёрстка не двигает.
        //
        // На грязной странице номер берётся от чистового набора — по тому же
        // символу. Страница на экране начинается не там, где чистовая, но
        // расходятся они меньше чем на страницу, и назвать читателю место в
        // книге это не мешает.
        const std::optional<std::size_t> first = cleanSpread();
        const std::wstring numbers =
            !first          ? std::wstring{L"…"}
            : drawn <= 1    ? std::format(L"{}", *first + 1)
                            : std::format(L"{}–{}", *first + 1, *first + drawn);
        const std::wstring total = book_->paginator().isComplete()
                                       ? std::format(L"{}", std::max<std::size_t>(pageCount(), 1))
                                       : std::wstring{L"…"};
        const std::wstring status =
            std::format(L"{} / {}     {:.0f}%", numbers, total, progress() * 100.0f);
        context->DrawText(status.c_str(), static_cast<UINT32>(status.size()), statusFormat_.Get(),
                          D2D1::RectF(margin, height - kVerticalMargin, width - margin, height),
                          dimBrush.Get());
    }
}

void BookView::drawInvitation(ID2D1DeviceContext* context, float width, float height) {
    const Theme& shade = paper();
    context->Clear(shade.background);
    drawBackdrop(context, width, height);

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
