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

/// Стороны разворота — индексы страниц-визуалов pages_.
constexpr std::size_t kLeftPage = 0;
constexpr std::size_t kRightPage = 1;

/// Кисть страницы-визуала для поверхности: тянет её на весь визуал
/// (UniformToFill, как прежде задник окна). В просвете быстрой растяжки страница
/// уже нового размера, а поверхность ещё старого, и без растяжения её край
/// сквозил бы на рабочий стол.
CompositionSurfaceBrush pageBrush(DrawingSurface const& surface) {
    CompositionSurfaceBrush brush = surface.brush();
    brush.stretch(CompositionStretch::UniformToFill);
    return brush;
}

/// Сколько листов переворота держим в воздухе разом. При быстром листании
/// каждый нажим пускает свой лист, и они летят внахлёст; больше этого числа не
/// копим — самый старый добивается рывком. Каждый лист — поверхность в размер
/// окна, то есть память (десяток с лишним листов — сотня-другая мегабайт),
/// поэтому число хоть и щедрое, но не бесконечное.
constexpr std::size_t kMaxFlips = 16;

/// Отпускание пула листов в простое. Первый лишний лист роняем через долгую
/// паузу — вдруг читатель тут же листнёт снова и пул понадобится сразу; дальше
/// по одному в секунду, пока не останется один (его держим на следующее
/// листание, чтобы не заводить поверхность заново). Так пик памяти держится
/// лишь на время листания, а в покое от пула остаётся один лист.
constexpr auto kFlipReleaseFirst = 10s;
constexpr auto kFlipReleaseStep = 1s;

/// Закон листания — пологая S-кривая (кубическая безье): небольшой разгон в
/// начале и торможение в конце, чтобы движение читалось живым, а не
/// равномерным. Контрольные точки симметричны относительно середины разворота;
/// концы мягкие, но скорость на них не гаснет в ноль — оттого «небольшой», а не
/// полная остановка на краях. Правится этими четырьмя числами.
constexpr float kEaseX1 = 0.30f, kEaseY1 = 0.10f;
constexpr float kEaseX2 = 0.70f, kEaseY2 = 0.90f;

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
///
/// На четверть легче прежнего: у корешка тень читалась провалом, а не
/// углублением, и съедала первые буквы строки. Той же парой меряется средник
/// разворота — он обязан совпасть с полутоном изгиба, иначе подмена в конце
/// переворота видна ступенькой.
constexpr std::uint32_t kFoldNear = 0x70000000;
constexpr std::uint32_t kFoldMid = 0x28000000;

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
constexpr float kEdgeOfWindow = 0.039f;
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

/// Подъём листа к глазу: во сколько раз свободный край выше собственной высоты
/// в верхней точке переворота. У корешка лист остаётся в своих размерах — там
/// он держится страницы и не поднимается вовсе, — а к свободному краю растёт, и
/// прямоугольник становится трапецией. Это и есть «ближе»: край, который
/// перелистывают, идёт к глазу, и глаз читает больший размер как меньшее
/// расстояние.
constexpr float kLiftPeak = 1.25f;

/// Выключка подъёма — своя, с погасшей скоростью на концах. Подъём состоит из
/// двух отрезков, вверх и вниз, и общая кривая листания (kEase*), у которой
/// концы намеренно не гаснут, дала бы на вершине излом — глаз читает такой
/// излом щелчком. Здесь лист трогается плавно, замирает наверху и так же
/// плавно ложится.
constexpr float kLiftEaseX1 = 0.50f, kLiftEaseY1 = 0.0f;
constexpr float kLiftEaseX2 = 0.50f, kLiftEaseY2 = 1.0f;

/// Трапеция листа — формула, которую композитор считает каждый кадр.
///
/// Аффинной матрицей трапеции не выйдет: сдвиг, поворот и растяжение сохраняют
/// параллельность сторон, и прямоугольник ими становится параллелограммом.
/// Нужен проективный переход — четвёртый столбец Matrix4x4, тот самый, которым
/// композиция делает перспективу: композитор делит на W после умножения, и
/// линейный по x знаменатель поднимает высоту тем сильнее, чем дальше от
/// корешка.
///
/// Вывод. Пусть корешок стоит в Hinge, свободный край отстоит от него на d, и
/// высота у края должна вырасти в k раз. В долях u = (x - Hinge)/d переход
///     W = 1 + p·u,   X = u·(1 + p),   Y = y - Half
/// при p = 1/k - 1 даёт ровно требуемое: у корешка (u = 0) единица, у края
/// (u = 1) высота в k раз. Раскрыв u обратно в x, получаем числа ниже: Slant —
/// это p, Fall — p/d, Half — середина листа по высоте, от которой он растёт в
/// обе стороны. Ширина при этом поджимается сама, как в перспективе: дальняя
/// от глаза половина листа занимает меньше места, чем ближняя, — этого не
/// избежать и не надо, ровно так выглядит наклонённая бумага.
///
/// Amount — доля подъёма, 0..1. Все шестнадцать чисел линейны по ней, поэтому
/// её одной довольно, чтобы вести трапецию во времени обычной скалярной
/// анимацией: матричной покадровой анимации в композиции нет.
constexpr wchar_t kLiftFormula[] =
    L"Matrix4x4("
    L"1 + lift.Amount * (Slant + Hinge * Fall), lift.Amount * Half * Fall, 0, lift.Amount * Fall,"
    L"0, 1, 0, 0,"
    L"0, 0, 1, 0,"
    L"-Hinge * lift.Amount * (Slant + Hinge * Fall), -Hinge * lift.Amount * Half * Fall, 0,"
    L"1 - Hinge * lift.Amount * Fall)";

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

/// Ставит трапецию на одну сторону листа: у `hinge` она остаётся в своих
/// размерах, у `freeEdge` вырастает в kLiftPeak раз — во столько, сколько
/// скажет Amount в `phase`.
///
/// Обе стороны листа — снимаемая бумага и приходящий лист — берут общий
/// корешок и свои, противоположные, свободные края. Оттого они и сходятся на
/// сгибе в одной высоте: точка на расстоянии s от корешка поднята одинаково, с
/// какой бы стороны бумаги она ни была, а сгиб — это одна точка, видимая с
/// обеих. Ничего согласовывать для этого не нужно, так выходит само.
void liftSheet(ExpressionAnimation const& warp, CompositionPropertySet const& phase,
               Visual const& visual, float hinge, float freeEdge, float height) {
    const float slant = 1.0f / kLiftPeak - 1.0f;
    warp.setReferenceParameter(L"lift", phase);
    warp.setScalarParameter(L"Slant", slant);
    warp.setScalarParameter(L"Fall", slant / (freeEdge - hinge));
    warp.setScalarParameter(L"Half", height * 0.5f);
    warp.setScalarParameter(L"Hinge", hinge);
    visual.startAnimation(L"TransformMatrix", warp);
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
      releaseTimer_(window.dispatcherQueue().createTimer()),
      // Всплывашка сноски — XAML-остров, её рисунок висит в дереве острова, а не
      // на сцене: ей нужен композитор острова, а не окна.
      note_(window.chromeCompositor()) {
    root_ = buildTree();

    // Таймер отпускания пула листов не повторяется — перезаводится сам с новой
    // паузой (armRelease/onReleaseTick).
    releaseTimer_.isRepeating(false);
    releaseTimer_.add_onTick(
        [this](wxl::Object const&, wxl::Object const&) { onReleaseTick(); });

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

    // Контейнер листов переворотов — над страницами. Клип контейнера не
    // даёт повёрнутому листу вылезти за полосу: клип живёт в координатах самого
    // визуала и применяется до его преобразования, поэтому повёрнутый лист
    // режется по прямоугольнику страницы, а не по описанному вокруг него. Сами
    // листы заводятся по требованию (makeFlip) — при быстром листании их в
    // воздухе несколько разом.
    sheets_ = compositor_.createContainerVisual();
    sheets_.value().clip(compositor_.createInsetClip());

    // Две страницы разворота — на сцене под листами, каждая в своей половине
    // окна. Размер и шов не наши: сцену (задний спрайт окна) wxl ресайзит
    // синхронно в WM_SIZE, а страницы берут у неё размер и половину ширины
    // выражениями композитора — те считаются на потоке DWM в том же кадре, и в
    // просвете быстрой растяжки окно остаётся покрытым без единого нашего
    // вызова (applySize их не трогает, своего Size у них нет). Половина ширины
    // — это и есть корешок: поля симметричны (см. spine()). Задник окна под
    // ними без кисти (setActive): красить под страницами нечего.
    ContainerVisual const scene = window_->contentVisual();
    ExpressionAnimation const sizeOfScene = compositor_.createExpressionAnimation(L"scene.Size");
    sizeOfScene.setReferenceParameter(L"scene", scene);
    ExpressionAnimation const halfOfScene =
        compositor_.createExpressionAnimation(L"scene.Size.X * 0.5");
    halfOfScene.setReferenceParameter(L"scene", scene);
    for (std::size_t side = 0; side < 2; ++side) {
        SpriteVisual page = compositor_.createSpriteVisual();
        InsetClip crop = compositor_.createInsetClip();
        crop.startAnimation(side == kLeftPage ? L"RightInset" : L"LeftInset", halfOfScene);
        page.clip(crop);
        page.startAnimation(L"Size", sizeOfScene);
        page.isVisible(false);
        scene.children().insertAtBottom(page);
        pages_[side] = page;
    }

    // Пул листов зарезервирован под предел: дальше push_back не переселяет
    // вектор, и указатели на листы, что держат обработчики конца переворота,
    // остаются годными.
    flips_.reserve(kMaxFlips);

    // Листы переворотов — на сцене окна над страницами, привешены к
    // contentVisual() (над задником, под островом), а не всунуты в дерево XAML
    // через setElementChildVisual. Пока полоса не стала текущим экраном, её
    // сцена скрыта — setActive(true) покажет при входе.
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
        // Бумагу вместе с набором несут страницы-визуалы на сцене, а задник
        // окна в чтении без кисти вовсе (см. setActive).
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

    // Страница в чтении — две страницы-визуала на сцене окна (pages_), а задник
    // окна без кисти: страницы кроют его целиком, и красить под ними нечего.
    // Входя, снимаем кисть и показываем страницы тем же кадром, что redraw
    // рисует разворот и одевает их. Уходя — наоборот: страницы прячутся, а
    // задник на промежуток до фона следующего экрана (заставка приходит
    // асинхронно) берёт осевший разворот сам — иначе окно сквозило бы на стол.
    if (active && window_) {
        window_->clearBackground();
        updateBackdrop();
        redraw();
    } else if (window_ && settled_) {
        cancelTurn();   // летящие садятся: заднику положен нынешний разворот, а не прошлый
        window_->background(*settled_);
    }
    for (auto const& page : pages_)
        if (page) page.value().isVisible(active);

    // Листы в покое не нужны: разворот видно страницами. Они выходят на сцену
    // только на время переворота — показывает их startTurn, прячут обратно
    // finishFlip и cancelTurn.
    if (sheets_) sheets_.value().isVisible(false);
}

void BookView::setTheme(int index) {
    const int count = themeCount();
    theme_ = ((index % count) + count) % count;
    note_.hide();   // подложка всплывашки покрашена прошлой темой
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
    core::nullable<XamlRoot> const xamlRoot = root_.value().xamlRoot();
    float scale = xamlRoot ? static_cast<float>(xamlRoot->rasterizationScale()) : 1.0f;
    if (scale <= 0.0f) scale = 1.0f;

    return applySize(width, height, scale);
}

bool BookView::applySize(float width, float height, float scale) {
    if (width == width_ && height == height_ && scale == scale_ && settled_) return false;

    width_ = width;
    height_ = height;
    scale_ = scale;

    const SizeInt32 pixels{static_cast<int32_t>(width * scale + 0.5f),
                           static_cast<int32_t>(height * scale + 0.5f)};
    if (pixels.width <= 0 || pixels.height <= 0) return false;

    // Задник и поверхности листов меняют размер, а не пересоздаются: кисти,
    // которые их уже показывают (фон окна, листы), продолжают показывать их же.
    if (!settled_)
        settled_.emplace(compositor_, pixels);
    else
        settled_->resize(pixels);

    // Листы пула — под новый размер: их поверхности и все зависящие от ширины
    // визуалы. Новые заведутся уже в этом размере (makeFlip).
    for (Flip& flip : flips_) {
        flip.surface->resize(pixels);
        flip.sheet.size({width, height});
        // Полоска тени сгиба меряется целым разворотом (её ужимает Scale по
        // ходу), приходящий лист — тоже; тени края и полутон изгиба — постоянной
        // ширины в долях окна.
        flip.fold.size({width, height});
        flip.leaf.size({width, height});
        flip.edge.size({width * kEdgeOfWindow, height});
        flip.bend.size({width * kBendOfWindow, height});
    }
    sheets_.value().size({width, height});

    // Окна кроя листов заданы в прежних числах и после смены размера
    // бессмысленны — все идущие перевороты в покой: доигрывать их по новым
    // размерам значило бы листать вслепую.
    cancelTurn();
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
    // от прерванных переворотов с окнами кроя, посчитанными по прежним числам.
    // Все идущие перевороты — в покой: доигрывать их по новым размерам значило
    // бы листать вслепую.
    cancelTurn();

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
    if (!settled_ || width_ <= 0.0f || height_ <= 0.0f) return;

    // Собираем показанный разворот из ленты колонок до отрисовки: дальше и
    // рисование, и попадание по сноске читают уже готовый spread_.
    buildSpread();

    // Посреди книжного переворота нынешний разворот несёт самый новый лист, а
    // не осевший: рисуем в его поверхность — перелистываемая страница и
    // приходящий лист смотрят на неё и обновятся сами, — а неперелистываемую
    // не трогаем: ей до посадки листа положено старое. Иначе глава,
    // досчитанная в полёте («из …» стало «из M»), одевала бы обе страницы в
    // новое, и летящий лист пропадал бы на глазах — на тех же точках под ним.
    if (settledStale_) {
        if (Flip* const newest = newestFlip()) drawSpread(*newest->surface);
        return;
    }

    drawSpread(*settled_);

    // Обе страницы — на осевший разворот. В чтении: до входа страницы скрыты,
    // а задником окна владеет заставка стартового экрана.
    if (active_) dressPages();
}

void BookView::dressPages() {
    // Одна кисть на обе страницы: поверхность одна, крой у каждой свой. Кисть
    // DrawingSurface на каждый вызов новая, поверхность за ней та же.
    CompositionSurfaceBrush const brush = pageBrush(*settled_);
    for (auto const& page : pages_) page.value().brush(brush);
}

void BookView::drawSpread(DrawingSurface& surface) {
    surface.draw([this](ID2D1DeviceContext* context) {
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
}

void BookView::turnPage(int delta) {
    if (!book_ || pageCount() == 0 || delta == 0)
        return;
    const bool forward = delta > 0;

    // Следующий разворот ленты от нынешнего места. Лента непрерывна, так что
    // это обычный шаг: границу главы он проходит сам, не прыжком. Место чтения
    // и страницы двигаются сразу, а лист летит вдогонку — поэтому быстрые
    // нажатия в одну сторону пускают несколько листов внахлёст, а не ждут в
    // очереди; встречное в режиме книги сперва сажает летящие (startTurn).
    Column target = anchorColumn();
    if (!ribbonSpread(target, forward))
        return;   // край книги — листать некуда

    startTurn(target, forward);
}

void BookView::startTurn(const Column& target, bool forward) {
    note_.hide();   // страница ушла, а сноска на ней осталась бы висеть

    // Режим книги — разворот в две колонки: там есть корешок и переворот листа
    // у него. Одна колонка и три с лишним — режим газеты: уезжает целая
    // страница. Страницы ведут себя в них по-разному, поэтому режим решаем
    // сразу.
    const bool book = columns_ == 2;

    // Встречное листание в режиме книги сперва сажает всё, что летит. Бумага
    // снимается с осевшего разворота, а у летящего листа перелистываемая
    // страница ещё в воздухе: снимать с неё нечего, и лист, пущенный навстречу
    // поверх летящих, показывал бы под собой то, чего в книге уже нет. Садятся
    // они рывком, в покой, осевший разворот — самого нового из них
    // (cancelTurn); листы в ту же сторону остаются лететь внахлёст.
    if (book)
        for (const Flip& f : flips_)
            if (f.active && f.forward != forward) {
                cancelTurn();
                break;
            }

    // Лист возьмётся свободный, новый или посаженный рывком — см. acquireFlip.
    Flip& flip = acquireFlip();
    flip.forward = forward;
    flip.epoch = flip.started = ++flipClock_;

    // Режим газеты: уезжающий лист несёт уходящую страницу, а из-под него
    // открываются страницы — новый разворот. Уходящее снимаем в лист ДО того,
    // как книга шагнёт: тогда на страницах ещё старое, а после шага — новое.
    if (!book)
        drawSpread(*flip.surface);

    // Двигаем книгу на целевой разворот. Целевая колонка может лежать в соседней
    // главе: делаем её текущей, не теряя вёрстки (buildSpread разложил её как
    // соседнюю на стыке).
    if (target.chapter != book_->currentChapter())
        book_->makeCurrentChapter(target.chapter);
    page_ = target.index;
    readingPosition_ = book_->paginator().page(page_).firstCharOffset;

    if (book) {
        // Режим книги: неперелистываемая страница (при листании вперёд левая)
        // остаётся на старом развороте, пока приходящий лист её не накроет.
        // Новых страниц у переворота две — оборот снимаемой бумаги и та, что
        // открывается под ней, — то есть ровно один новый разворот: собираем
        // его и рисуем в поверхность листа, единственной отрисовкой на
        // переворот. Из неё animateSpreadTurn оденет перелистываемую страницу
        // и выведет приходящий лист; старое лист заимствует у страницы, на
        // которой оно уже показано. Осевший разворот (settled_) лист, сев,
        // сменит обменом поверхностей, без отрисовки (landFlip), — до того
        // нынешний разворот несёт самый новый лист, а не settled_
        // (settledStale_).
        buildSpread();
        drawSpread(*flip.surface);
        settledStale_ = true;
    } else {
        // Режим газеты: страницы — новый разворот, его и открывает уезжающий
        // лист.
        redraw();
    }

    if (sheets_) sheets_.value().isVisible(true);   // листы — на время переворота

    if (book)
        animateSpreadTurn(flip, forward);
    else
        animateTurn(flip, forward);

    if (onPositionChanged) onPositionChanged(readingPosition_);
}

BookView::Flip& BookView::acquireFlip() {
    releaseTimer_.stop();   // снова листают — отпускание пула отменяется

    // Свободный лист в пуле?
    for (Flip& f : flips_)
        if (!f.active) {
            f.active = true;
            return f;
        }

    // Ещё не набрали лимит — заводим новый. Пул зарезервирован под kMaxFlips
    // (buildTree), так что push_back не переселяет вектор: указатели на листы,
    // что держат обработчики конца, остаются годными.
    if (flips_.size() < kMaxFlips) {
        flips_.push_back(makeFlip());
        Flip& fresh = flips_.back();
        fresh.active = true;
        applyShadowTint();   // покрасить его тени под нынешнюю тему
        return fresh;
    }

    // Все в воздухе, а листать просят ещё — сажаем самый старый рывком. Сев,
    // он отдаёт свой разворот неперелистываемой странице как доигравший
    // (landFlip): иначе из-под ещё летящих на миг выглянуло бы то, что было
    // до него.
    Flip* oldest = &flips_.front();
    for (Flip& f : flips_)
        if (f.started < oldest->started)
            oldest = &f;
    oldest->epoch = ++flipClock_;   // его обработчик конца, придя, увидит чужой номер
    finishFlip(*oldest);
    landFlip(*oldest);
    oldest->active = true;
    return *oldest;
}

BookView::Flip* BookView::newestFlip() {
    Flip* newest = nullptr;
    for (Flip& f : flips_)
        if (!newest || f.started > newest->started)
            newest = &f;
    return newest;
}

BookView::Flip BookView::makeFlip() {
    using namespace wxl::dsl;   // colors.transparent — как в buildTree

    const SizeInt32 pixels{static_cast<int32_t>(width_ * scale_ + 0.5f),
                           static_cast<int32_t>(height_ * scale_ + 0.5f)};
    DrawingSurface surface(compositor_, pixels);

    SpriteVisual sheet = compositor_.createSpriteVisual();
    sheet.brush(surface.brush());
    sheet.size({width_, height_});
    // Крой в покое отпущен на вылет тени: нулевые отступы — ровно лист, а тени
    // положено лежать за его краем.
    InsetClip clip = compositor_.createInsetClip(-kShadowReach, -kShadowReach, -kShadowReach,
                                                 -kShadowReach);
    sheet.clip(clip);
    sheet.isVisible(false);

    // Тень уезжающего листа — маской ей кисть листа, поэтому тень повторяет
    // лист, а не описанный прямоугольник. Гасится и зажигается прозрачностью.
    DropShadow shadow = compositor_.createDropShadow();
    shadow.blurRadius(kShadowBlur);
    shadow.offset({kShadowShift, 0.0f, 0.0f});
    shadow.mask(sheet.brush());
    shadow.opacity(0.0f);
    sheet.shadow(shadow);

    // Тень сгиба (книжное листание): горизонтальный градиент в долях своей
    // ширины, поэтому полоске достаточно ездить — перекрашивать не приходится.
    CompositionLinearGradientBrush foldBrush = compositor_.createLinearGradientBrush();
    foldBrush.startPoint({0.0f, 0.0f});
    foldBrush.endPoint({1.0f, 0.0f});
    CompositionColorGradientStop foldMid =
        compositor_.createColorGradientStop(kFoldMidStop, colors.transparent);
    foldBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    foldBrush.colorStops().append(foldMid);
    foldBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));
    SpriteVisual fold = compositor_.createSpriteVisual();
    fold.brush(foldBrush);
    fold.size({width_, height_});
    fold.isVisible(false);

    // Тень наружного края приходящего листа: узкая и неизменная, градиент
    // развёрнут — густо у листа, прозрачно прочь.
    CompositionLinearGradientBrush edgeBrush = compositor_.createLinearGradientBrush();
    edgeBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    edgeBrush.colorStops().append(compositor_.createColorGradientStop(kEdgeMidStop, colors.transparent));
    edgeBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));
    SpriteVisual edge = compositor_.createSpriteVisual();
    edge.brush(edgeBrush);
    edge.size({width_ * kEdgeOfWindow, height_});
    edge.isVisible(false);

    // Приходящий лист (кисть выдаётся на каждый переворот — задника) и полутон
    // изгиба у сгиба: ребёнок листа, кроится вместе с ним.
    SpriteVisual leaf = compositor_.createSpriteVisual();
    InsetClip leafClip = compositor_.createInsetClip();
    leaf.clip(leafClip);
    leaf.size({width_, height_});
    leaf.isVisible(false);
    CompositionLinearGradientBrush bendBrush = compositor_.createLinearGradientBrush();
    bendBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    bendBrush.colorStops().append(compositor_.createColorGradientStop(kBendMidStop, colors.transparent));
    bendBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));
    SpriteVisual bend = compositor_.createSpriteVisual();
    bend.brush(bendBrush);
    bend.size({width_ * kBendOfWindow, height_});
    leaf.children().insertAtTop(bend);

    // Подъём листа к глазу (книжное листание): доля подъёма — скаляр в
    // собственных свойствах листа, а две трапеции читают её выражениями. Числа
    // геометрии ставит начало каждого переворота: корешок и свободный край
    // зависят от стороны листания, а размеры — от окна.
    CompositionPropertySet lift = sheet.properties();
    lift.insertScalar(L"Amount", 0.0f);
    ExpressionAnimation sheetLift = compositor_.createExpressionAnimation(kLiftFormula);
    ExpressionAnimation leafLift = compositor_.createExpressionAnimation(kLiftFormula);

    // Все листовые визуалы — в контейнер; их Z на каждый переворот уточняет
    // анимация (плоское — под старые, книжное — над старыми).
    VisualCollection const children = sheets_.value().children();
    children.insertAtTop(sheet);
    children.insertAtTop(fold);
    children.insertAtTop(edge);
    children.insertAtTop(leaf);

    return Flip{std::move(surface), sheet,     clip,      shadow,  leaf, leafClip, bend,
                bendBrush,          fold,      foldBrush, foldMid, edge, edgeBrush,
                lift,               sheetLift, leafLift};
}

void BookView::finishFlip(Flip& flip) {
    // Сначала снять анимации, потом писать: пока анимация на свойстве жива,
    // прямая запись до него не доходит, и недоехавший лист доехал бы позже уже
    // не к месту.
    flip.sheet.stopAnimation(L"Offset");
    flip.sheet.stopAnimation(L"RotationAngleInDegrees");
    flip.sheet.offset({0.0f, 0.0f, 0.0f});
    flip.sheet.rotationAngleInDegrees(0.0f);
    flip.sheet.isVisible(false);

    flip.clip.stopAnimation(L"LeftInset");
    flip.clip.stopAnimation(L"RightInset");
    flip.clip.leftInset(-kShadowReach);
    flip.clip.rightInset(-kShadowReach);
    flip.clip.topInset(-kShadowReach);
    flip.clip.bottomInset(-kShadowReach);

    flip.shadow.opacity(0.0f);

    flip.fold.stopAnimation(L"Offset");
    flip.fold.stopAnimation(L"Scale");
    flip.fold.stopAnimation(L"Opacity");
    flip.fold.isVisible(false);
    flip.foldMid.stopAnimation(L"Offset");

    flip.edge.stopAnimation(L"Offset");
    flip.edge.isVisible(false);

    flip.leaf.stopAnimation(L"Offset");
    flip.leaf.isVisible(false);
    flip.bend.stopAnimation(L"Offset");
    flip.bend.stopAnimation(L"Opacity");
    flip.bend.opacity(1.0f);
    flip.leafClip.stopAnimation(L"LeftInset");
    flip.leafClip.stopAnimation(L"RightInset");

    // Подъём снимается с обеих сторон листа, и трапеция сходит в тождественную.
    // Не только ради вида застывшего листа: выражение, оставленное на свойстве,
    // композитор считает каждый кадр — за все свободные листы пула и без
    // всякой нужды.
    flip.lift.stopAnimation(L"Amount");
    flip.lift.insertScalar(L"Amount", 0.0f);
    flip.sheet.stopAnimation(L"TransformMatrix");
    flip.sheet.transformMatrix(identity_matrix());
    flip.leaf.stopAnimation(L"TransformMatrix");
    flip.leaf.transformMatrix(identity_matrix());

    flip.active = false;
}

void BookView::landFlip(Flip& flip) {
    // Режим газеты: лист нёс уходящий разворот, и он больше никому не нужен —
    // осевший сменился ещё в startTurn (флаг не взведён).
    if (!settledStale_) return;

    // Режим книги: лист нёс новый разворот, и теперь тот — осевший. Не рисуя:
    // осевший разворот меняется с листом поверхностями, а прежняя уходит листу
    // под следующее листание. Так на пачку листов приходится ровно по одной
    // отрисовке на лист и ни одной сверх; кисти листа, глядевшие на его прежнюю
    // поверхность, анимация переставляет на каждом перевороте заново.
    //
    // Неперелистываемая страница переезжает на него сейчас, а не когда сядут
    // все: приходящий лист только что лёг на неё ровно этими точками (подмены
    // не видно — тот же кадр, что и сокрытие листа), а следующий лист, ещё
    // летящий, ложится уже на них. Ждать последнего значило бы показать из-под
    // него то, что было до первого. Перелистываемая страница носит поверхность
    // самого нового листа и здесь не трогается: когда сядет и он, обмен
    // положит ту же поверхность в settled_ (settleSheets).
    std::swap(*settled_, *flip.surface);
    if (active_)
        pages_[flip.forward ? kLeftPage : kRightPage].value().brush(pageBrush(*settled_));
}

void BookView::settleSheets() {
    for (Flip& f : flips_)
        if (f.active) return;

    // Все листы сели. В режиме книги осевший разворот — разворот последнего
    // севшего, то есть самого нового (landFlip), и обе страницы уже на нём:
    // неперелистываемую одел landFlip, а перелистываемая носит поверхность
    // самого нового листа — ту самую, что обменом легла в settled_. Одевать
    // заново нечего: осевший разворот — нынешний.
    settledStale_ = false;

    if (sheets_) sheets_.value().isVisible(false);
    armRelease();   // все листы свободны — отпускать пул по таймеру
}

void BookView::cancelTurn() {
    // Все идущие перевороты — в покой рывком. Пакеты их концов ещё придут, но
    // каждому листу здесь меняется epoch, и обработчик, придя, увидит чужой
    // номер и промолчит: остановленная анимация закрывает пакет так же, как
    // доигравшая. Садится при этом один самый новый: его разворот — нынешний,
    // а развороты остальных пропущены — в книге их уже нет.
    Flip* newest = nullptr;
    for (Flip& f : flips_)
        if (f.active) {
            f.epoch = ++flipClock_;
            finishFlip(f);
            if (!newest || f.started > newest->started) newest = &f;
        }
    if (newest) landFlip(*newest);
    settleSheets();   // все сели: листы скрыть, пул — на отпускание
}

void BookView::armRelease() {
    // Один лист держим всегда — на следующее листание, чтобы не заводить
    // поверхность заново. Отпускать нечего, пока в пуле не больше одного.
    if (flips_.size() <= 1) return;
    releaseTimer_.stop();
    releaseTimer_.interval(kFlipReleaseFirst);
    releaseTimer_.start();
}

void BookView::onReleaseTick() {
    releaseTimer_.stop();

    // Снова листают — пул нужен, ничего не трогаем. acquireFlip таймер уже
    // остановил, но тик мог уйти в очередь раньше остановки.
    for (const Flip& f : flips_)
        if (f.active) return;

    // Отпускаем один лишний лист: снимаем его визуалы со сцены и роняем — с ним
    // уходит и его поверхность в размер окна. Берём последний: все свободны,
    // порядок не важен, а pop_back остальных не двигает. Последний, один, лист
    // оставляем — на следующее листание.
    if (flips_.size() > 1) {
        Flip& f = flips_.back();
        VisualCollection const children = sheets_.value().children();
        children.remove(f.sheet);
        children.remove(f.fold);
        children.remove(f.edge);
        children.remove(f.leaf);   // полутон изгиба — ребёнок листа, уходит с ним
        flips_.pop_back();
    }

    // Ещё есть лишние — следующий через секунду.
    if (flips_.size() > 1) {
        releaseTimer_.interval(kFlipReleaseStep);
        releaseTimer_.start();
    }
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

    // Тени у каждого листа пула свои — красим все. Тень уезжающего листа не
    // градиент, а размытие: тон непрозрачный, прозрачностью правит сама тень.
    for (Flip& f : flips_) {
        paint(f.foldBrush.colorStops(), kFoldNear, kFoldMid);
        paint(f.edgeBrush.colorStops(), kEdgeNear, kEdgeMid);
        paint(f.bendBrush.colorStops(), kBendNear, kBendMid);
        f.shadow.color(tinted(hue, 1.0f));
    }
}

void BookView::animateTurn(Flip& flip, bool forward) {
    // Уезжающий лист несёт уходящую страницу из собственной поверхности. Лист
    // мог прежде служить в режиме книги, где его кисть переставлена на чужую
    // поверхность, а своя ушла осевшему развороту обменом при оседании, —
    // ставим нынешнюю свою и ей же маскируем тень: в газете лист открывает
    // страницы под собой, а не повторяет их.
    flip.sheet.brush(flip.surface->brush());
    flip.shadow.mask(flip.sheet.brush());

    // Поворот идёт вокруг левого края: там корешок, оттуда лист и поднимается.
    // Ставится здесь, а не раз на лист: центр преобразования принадлежит
    // плоскому листанию, а книжное его обнуляет — там лист не поворачивается, а
    // гнётся трапецией от собственного начала координат.
    flip.sheet.centerPoint({0.0f, height_ * 0.5f, 0.0f});

    // Плоское листание: новые листы — под старыми. Только что заведённый лист
    // кладём в самый низ контейнера, над страницами; уже летящие остаются выше
    // и уезжают первыми, открывая тех, что под ними.
    VisualCollection const children = sheets_.value().children();
    children.remove(flip.sheet);
    children.insertAtBottom(flip.sheet);

    // Уезжает копия уходящего разворота (flip.sheet), открывая задник — новый
    // разворот. Уехать надо дальше собственной ширины: лист поворачивается
    // вокруг левого края, и его дальний нижний угол отходит не на ширину, а на
    // гипотенузу (ширина·cos + полвысоты·sin); плюс тень, сдвинутую вправо и
    // размытую, иначе она осталась бы у края серой полоской. Вперёд лист уходит
    // влево, назад — вправо.
    const float reach = width_ * kTurnCos + height_ * 0.5f * kTurnSin + kShadowShift + kShadowBlur;
    const float to = forward ? -reach : reach;
    const float angleTo = forward ? -kTurnAngle : kTurnAngle;

    auto const easing =
        compositor_.createCubicBezierEasingFunction({kEaseX1, kEaseY1}, {kEaseX2, kEaseY2});

    auto slide = compositor_.createVector3KeyFrameAnimation();
    slide.duration(kTurn);
    slide.insertKeyFrame(0.0f, Vector3{0.0f, 0.0f, 0.0f}, easing);
    slide.insertKeyFrame(1.0f, Vector3{to, 0.0f, 0.0f}, easing);

    auto turn = compositor_.createScalarKeyFrameAnimation();
    turn.duration(kTurn);
    turn.insertKeyFrame(0.0f, 0.0f, easing);
    turn.insertKeyFrame(1.0f, angleTo, easing);

    // Тень — не украшение: лист и страница под ним одного цвета, и без тени
    // глаз не видит, что один поднят над другим.
    flip.shadow.opacity(kShadowOpacity);
    flip.sheet.isVisible(true);

    // Конец переворота отслеживается пакетом: по нему лист освобождается в пул.
    // Пакет закрывает и остановленную анимацию, поэтому обработчик проверяет и
    // жизнь полосы, и свой ли это лист (epoch).
    const std::uint32_t epoch = flip.epoch;
    Flip* const which = &flip;
    auto batch = compositor_.createScopedBatch(CompositionBatchTypes::Animation);
    flip.sheet.startAnimation(L"Offset", slide);
    flip.sheet.startAnimation(L"RotationAngleInDegrees", turn);
    batch.add_onCompleted([this, alive = std::weak_ptr<int>(alive_), which, epoch](
                              Object const&, CompositionBatchCompletedEventArgs&) {
        if (alive.expired() || which->epoch != epoch) return;
        finishFlip(*which);
        landFlip(*which);
        settleSheets();
    });
    batch.end();
}

void BookView::animateSpreadTurn(Flip& flip, bool forward) {
    // Уходит старый разворот, и в обе стороны он остаётся сверху: книжное
    // листание снимает верхнюю бумагу с неподвижной стопки, а не увозит
    // страницу за край. У переворота две стороны разной давности: под снимаемой
    // бумагой с первого же кадра открывается НОВАЯ страница, а неперелистываемая
    // остаётся СТАРОЙ, пока приходящий лист её не накроет. Для того страницы и
    // два визуала: перелистываемую переставляем на новый разворот сейчас,
    // другую — при оседании (settleSheets).
    SpriteVisual const& going = flip.sheet;
    InsetClip const& goingCrop = flip.clip;
    SpriteVisual const& coming = flip.leaf;
    InsetClip const& comingCrop = flip.leafClip;
    SpriteVisual const& fold = flip.fold;
    SpriteVisual const& rim = flip.edge;
    SpriteVisual const& flipping = pages_[forward ? kRightPage : kLeftPage].value();

    // Уходящий лист несёт СТАРУЮ перелистываемую страницу — ту, что показана
    // прямо сейчас: её кисть и забираем у страницы-визуала, прежде чем одеть ту
    // в новое. Своей отрисовки у уходящего нет, и какой из летящих листов или
    // осевший разворот эту кисть дал — неважно: страница уже носит верную.
    going.brush(flipping.brush());

    // Обе НОВЫЕ страницы — из поверхности листа, куда startTurn нарисовал новый
    // разворот: перелистываемая страница-визуал одевается в неё с первого кадра
    // (из-под снимаемой бумаги открывается новое), приходящий лист несёт ту,
    // что ляжет на неперелистываемую. Кисть DrawingSurface делает новую на
    // каждый вызов, а поверхность за ней та же — второй отрисовки не возникает.
    flipping.brush(pageBrush(*flip.surface));
    coming.brush(flip.surface->brush());

    // Стопка листов: снимаемая бумага нового листа — ПОД всеми летящими, а
    // приходящий лист — НАД ними. Летящие подняты раньше и висят выше, так что
    // в правой половине сверху лежит самый старый — и его тень сгиба падает на
    // бумагу, которую снимают следом; а снятый последним ляжет на левую стопку
    // последним, сверху. Первая версия клала весь новый лист поверх старых, и
    // читатель видел это как «нижняя страница вылезла на передний план»: его
    // бумага — те же точки, что только что показывала правая страница, — с
    // первого кадра накрывала всё, что летело. Друг друга по развороту листы
    // не затирают, потому что каждый несёт лишь свою перелистываемую страницу
    // (см. крой ниже), а не весь разворот. Тень сгиба — под бумагой, тень
    // наружного края — под приходящим листом.
    VisualCollection const children = sheets_.value().children();
    children.remove(going);
    children.insertAtBottom(going);
    children.remove(fold);
    children.insertAtBottom(fold);
    children.remove(rim);
    children.insertAtTop(rim);
    children.remove(coming);
    children.insertAtTop(coming);

    const float leftPage = spine();
    const float rightPage = width_ - leftPage;

    // Кромка уходящего листа доходит до корешка: вперёд едет правая, назад —
    // левая.
    const float travel = forward ? rightPage : leftPage;
    const wchar_t* const inset = forward ? L"RightInset" : L"LeftInset";

    // Лист несёт только перелистываемую страницу: вперёд — правую (левую половину
    // разворота отрезаем к корешку неподвижным отступом), назад — левую. Иначе
    // неперелистываемая половина каждого листа затирала бы соседние листы при
    // быстром листании внахлёст; за неё отвечает своя страница-визуал.
    if (forward)
        goingCrop.leftInset(leftPage);
    else
        goingCrop.rightInset(rightPage);

    // Лист поднимается к глазу: у корешка он в своих размерах, к свободному
    // краю растёт трапецией. Корешок у обеих сторон общий, а свободные края
    // противоположны — это один и тот же край бумаги, только у снимаемой
    // стороны он ещё снаружи перелистываемой страницы, а у приходящей уже
    // перевёрнут на другую сторону разворота. Центр преобразования при этом
    // обнуляется: трапеция задана от начала координат листа, а центр нужен
    // только плоскому листанию, где вокруг него идёт поворот.
    going.centerPoint({0.0f, 0.0f, 0.0f});
    liftSheet(flip.sheetLift, flip.lift, going, leftPage, forward ? width_ : 0.0f, height_);
    liftSheet(flip.leafLift, flip.lift, coming, leftPage, forward ? 0.0f : width_, height_);

    // Пологая S-кривая: рука, тянущая бумагу, слегка разгоняется в начале и
    // тормозит к корешку — не роняет тяжесть, но и не тянет мёртво-равномерно
    // (kEase*).
    auto const easing =
        compositor_.createCubicBezierEasingFunction({kEaseX1, kEaseY1}, {kEaseX2, kEaseY2});

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

    flip.foldBrush.startPoint({forward ? 0.0f : 1.0f, 0.0f});
    flip.foldBrush.endPoint({forward ? 1.0f : 0.0f, 0.0f});

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

    flip.edgeBrush.startPoint({forward ? 1.0f : 0.0f, 0.0f});
    flip.edgeBrush.endPoint({forward ? 0.0f : 1.0f, 0.0f});

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

    flip.bendBrush.startPoint({forward ? 1.0f : 0.0f, 0.0f});
    flip.bendBrush.endPoint({forward ? 0.0f : 1.0f, 0.0f});

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

    // Доля подъёма — вверх к середине переворота и обратно вниз. Ею одной
    // ведутся обе трапеции: все числа матрицы линейны по этой доле, и
    // композитор пересчитывает их сам (kLiftFormula).
    auto const liftEasing = compositor_.createCubicBezierEasingFunction(
        {kLiftEaseX1, kLiftEaseY1}, {kLiftEaseX2, kLiftEaseY2});

    auto rise = compositor_.createScalarKeyFrameAnimation();
    rise.duration(kLeafSlide);
    rise.insertKeyFrame(0.0f, 0.0f, liftEasing);
    rise.insertKeyFrame(0.5f, 1.0f, liftEasing);
    rise.insertKeyFrame(1.0f, 0.0f, liftEasing);

    going.isVisible(true);
    fold.isVisible(true);
    rim.isVisible(true);
    coming.isVisible(true);

    // Конец отслеживается пакетом: по нему лист освобождается в пул. Пакет
    // закрывает и остановленную анимацию (лист добили при быстром листании или
    // сбросили при перевёрстке), поэтому обработчик проверяет и жизнь полосы, и
    // свой ли это лист — по epoch: у добитого он уже сменился, и обработчик
    // узнаёт свой номер чужим и молчит.
    const std::uint32_t epoch = flip.epoch;
    Flip* const which = &flip;

    auto batch = compositor_.createScopedBatch(CompositionBatchTypes::Animation);

    goingCrop.startAnimation(inset, crawl);
    fold.startAnimation(L"Offset", follow);
    fold.startAnimation(L"Scale", widen);
    fold.startAnimation(L"Opacity", lighten);
    flip.foldMid.startAnimation(L"Offset", flatten);
    rim.startAnimation(L"Offset", trail);
    coming.startAnimation(L"Offset", slide);
    comingCrop.startAnimation(opening, open);
    flip.bend.startAnimation(L"Offset", curve);
    flip.bend.startAnimation(L"Opacity", settle);
    flip.lift.startAnimation(L"Amount", rise);

    batch.add_onCompleted([this, alive = std::weak_ptr<int>(alive_), which, epoch](
                        Object const&, CompositionBatchCompletedEventArgs&) {
        if (alive.expired() || which->epoch != epoch) return;
        finishFlip(*which);
        landFlip(*which);
        settleSheets();
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
