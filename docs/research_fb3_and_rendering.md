# FB3-читалка под Windows 10/11: исследование формата, библиотек и рендеринга текста

Дата исследования: 22.08.2026. Целевой стек: **C++ / WinRT**.

---

## Резюме (TL;DR)

1. **Формат FB3** — это ZIP-контейнер по стандарту **OPC (ECMA-376 Part 2**, как docx/xlsx): `[Content_Types].xml` + `.rels`-связи + отдельные части `description.xml`, `body.xml` и файлы картинок. Тело — строгий семантический XML (без CSS), словарь элементов небольшой и хорошо документирован XSD-схемами. Спецификация живёт на GitHub: [gribuser/FB3](https://github.com/gribuser/FB3) (там же — три официальных примера `.fb3`, включая стресс-тесты).
2. **Готовых библиотек мало**: официальный Perl-инструментарий LitRes (актуален, FB3-Convert v0.48 от апреля 2026), эталонный TypeScript-рендерер [Litres/FB3Reader](https://github.com/Litres/FB3Reader), и **единственная C++-реализация — в crengine** (`fb3fmt.cpp` + `lvopc.cpp`, ~260 строк формата поверх маленького OPC-хелпера; GPL — изучать можно, копировать нельзя). На Python/Rust/C#/Go библиотек FB3 не существует — своя C++ реализация будет по сути первой не-GPL нативной.
3. **Парсинг на C++ прост**: у Windows есть **нативный COM OPC API** (`IOpcFactory`/`IOpcPackage`, msopc.dll) — контейнер FB3 читается им «из коробки»; альтернатива — libzip/minizip-ng + pugixml (сам OPC-слой — это ~200–300 строк).
4. **Рендеринг текста**: правильный путь под Windows — **DirectWrite + Direct2D + DirectComposition**. Для книжной типографики (переносы + выключка) нужен кастомный layout на `IDWriteTextAnalyzer` (шейпинг/фолбэк остаются на DirectWrite, свой только перенос строк). Переносы — [hunspell/hyphen](https://github.com/hunspell/hyphen) (алгоритм Ляна/TeX) со словарями `hyph_ru_RU`/`hyph_en_US`. Скорость не проблема: целый роман пагинируется за <1 секунду (данные Петцольда).
5. **Архитектурный образец — crengine** (компактный DOM + «layout один раз → нарезка на страницы» + кэш-файл + позиции-xpointer’ы), а не встраивание веб-движка: FB3 — ограниченный семантический формат, для которого собственный layout-движок — обозримая задача, и только он даёт постраничные сноски, точные закладки и мгновенное переоткрытие.

---

## 1. Формат FB3

### 1.1 Статус и происхождение

- Автор — Дмитрий Грибов (создатель FB2, экс-CTO ЛитРес). Канонический источник — репозиторий [gribuser/FB3](https://github.com/gribuser/FB3): **прозаической спецификации нет**, формат определён XSD-схемами + автогенерированной документацией (`schema_docs/`) + официальными примерами.
- Формат жив как **внутренний мастер-формат ЛитРес** (книги в приложениях «Литрес» хранятся как `.fb3`; из FB3 генерируются fb2/epub/mobi/pdf), но как публичный формат обмена не прижился: calibre его не поддерживает ([открытый тикет с 2018](https://bugs.launchpad.net/bugs/1808532)), FBReader — нет, поддержка есть в CoolReader/KOReader/crengine-ng и некоторых ONYX BOOX.
- Инструментарий обновляется до сих пор: FB3-Convert v0.48 вышел **23.04.2026** ([CPAN](https://metacpan.org/dist/FB3-Convert)).
- Лицензия схем: BSD-подобная от ЛитРес с оговоркой — **схемы нельзя модифицировать** (использовать и распространять без изменений — можно). Perl-инструменты — LGPL.

### 1.2 Структура контейнера (OPC)

`.fb3` — обычный ZIP, организованный по Open Packaging Conventions. Канонический layout (из официального примера):

```
/[Content_Types].xml
/_rels/.rels
/meta/core.xml                  ← OPC core properties (Dublin Core: title, creator…)
/fb3/description.xml            ← метаданные FB3
/fb3/body.xml                   ← текст книги
/fb3/_rels/description.xml.rels
/fb3/_rels/body.xml.rels
/fb3/img/*.png|jpg|jpeg|gif|svg ← картинки обычными файлами
```

Ключевые константы:

| Что | Значение |
|---|---|
| Content type тела | `application/fb3-body+xml` |
| Content type описания | `application/fb3-description+xml` |
| Связь пакета → description | `http://www.fictionbook.org/FictionBook3/relationships/Book` (заглавная B!) |
| Связь description → body | `http://www.fictionbook.org/FictionBook3/relationships/body` |
| Связь body → картинка | `http://www.fictionbook.org/FictionBook3/relationships/image` |
| Обложка | стандартная OPC-связь thumbnail: `http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail` |

**Важно:** имена частей НЕ фиксированы — фиксированы только типы связей и content types. Официальный пример «Hardcore file structure» намеренно ломает всё: части с кириллическими именами (`/фб3/…`, `/книга` без расширения), верхний регистр в `_RELS`, картинка `j.o.p.a.jPeG`, объявленная как `image/svg+xml`. Корректный ридер обязан делать полное OPC-разрешение: `[Content_Types].xml` → `/_rels/.rels` → цепочка связей, без хардкода путей. На практике ЛитРес выпускает простую структуру выше.

`<img src="…">` в body ссылается **на Id связи** в `.rels`-файле, а не на путь к файлу.

### 1.3 Схемы и модель тела

XSD в корне репозитория: `fb3_body.xsd`, `fb3_descr.xsd`, `fb3_general.xsd` (UUID), `fb3_links.xsd` (xlink), `fb3_relations.xsd`, `fb3_descr_classes.xsd` + OPC/DC-схемы. Пространства имён: `http://www.fictionbook.org/FictionBook3/body` и `…/description`.

Корень тела: `<fb3-body id="{UUID}">` → `title?`, `epigraph*`, `section+`, `notes*`. На уровне XSD есть key/keyref: id секций уникальны, каждая `<note href>` обязана разрешаться в `notebody`.

Блочные элементы: `section` (вложенные, id-UUID, атрибуты `article`, `doi`, `clipped`, `output` для триал-фрагментов), `p`, `subtitle`, **настоящие списки `ol/ul/li`** (вложенные), `pre`, `table` (`th/td` + colspan/rowspan/align/valign), `poem`/`stanza`, `blockquote`, `annotation`, `epigraph`, `subscription`, `marker` и — главное новшество — **`<div>` (DivBlockType)**: `float=left|right|center`, `width/min-width/max-width` в физических единицах (`em|ex|%|mm`), `align`, `border`, `on-one-page` (keep-together), `bindto` (привязка плавающего блока к элементу).

Инлайн (`StyleType`): `strong`, `em`, `strikethrough`, `sub`, `sup`, `code`, `underline`, `spacing` (разрядка), `smallcaps`, `span class="…"` (свободный класс — точка привязки стилей), `a xlink:href`, `note`, `img`, `paper-page-break` (соответствие бумажным страницам).

**Сноски** — сильная сторона формата: `<note href="n_1" xlink:role="footnote|endnote|comment|auto|other" autotext="1|i|a|*|keep">` → `<notebody id="n_1">` внутри `<notes show="0|1">`; поддерживаются раздельные блоки подстрочных сносок и комментариев, авто-нумерация с выбором стиля.

`description.xml`: заголовок, серии, ~40 ролей связанных персон (author/translator/narrator/…), многомерная классификация (class/subject/target-audience/setting + УДК/ББК), `written`, `document-info`, `draft-status` (поддержка «книг-сериалов»: ожидаемый объём, частота обновлений), `paper-publish-info`, `annotation`, `fb3-fragment`. Всё на UUID; пара `id`+`version` определяет эквивалентность документов.

### 1.4 Отличия от FB2 (кратко)

Контейнер OPC вместо одного XML; картинки отдельными файлами (+SVG и GIF) вместо base64-`<binary>`; метаданные читаются без парсинга текста; UUID вместо свободных id; настоящие списки и таблицы; `div`-флоаты с физическими размерами; `underline`/`spacing`/`smallcaps`/`span class`; типизированные сноски с авто-нумерацией; первоклассные триал-фрагменты (`output=trial|payed`, `clipped` с сохранением глобальных позиций символов); `draft-status`; привязка к бумажным страницам. Философия сохранена: **семантическая, а не визуальная разметка** (в отличие от EPUB) — CSS в формате нет.

### 1.5 Тестовый корпус

В [Examples/](https://github.com/gribuser/FB3/tree/master/Examples) — три официальных файла (и в виде `.fb3`, и распакованные):
- `Anathomy tutorial example.fb3` — чистая каноническая структура;
- `nightmare_example.fb3` — стресс-тест разметки: все типы сносок, div-флоаты/bindto/рамки, span-классы, paper-page-break, clipped-секции, SVG/GIF;
- `Hardcore file structure.fb3` — патологический OPC.

---

## 2. Существующие реализации и библиотеки

### 2.1 Сводная таблица

| Проект | Язык | Лицензия | Что делает | Статус | URL |
|---|---|---|---|---|---|
| gribuser/FB3 | XSD/Perl | BSD-like (схемы неизменяемы) | схемы, доки, примеры, Perl API | активен (04.2026) | [github](https://github.com/gribuser/FB3) |
| FB3 (CPAN) | Perl | LGPL 2.1 | парсинг + **XSD-валидация** контейнера | v0.17, 2021 | [metacpan](https://metacpan.org/dist/FB3) |
| FB3-Convert (CPAN) | Perl | LGPL 2.1 | **FB2→FB3, EPUB→FB3, FB3→FB2, FB3→JSON**, вырезка триалов | v0.48, 04.2026 — активен | [metacpan](https://metacpan.org/dist/FB3-Convert) |
| Litres/FB3Reader | TypeScript | LGPL | **рендеринг/пагинация FB3** в браузере (движок веб-читалки ЛитРес) | заморожен (2023), 75★ | [github](https://github.com/Litres/FB3Reader) |
| Litres/FB3Editor | JS+Perl | BSD-2 | онлайн-редактор FB3 | малоактивен | [github](https://github.com/Litres/FB3Editor) |
| **crengine (buggins/coolreader)** | **C++** | GPL-2+ | **детект/парсинг/рендеринг FB3**: `fb3fmt.cpp` (260 строк) + OPC-реализация `lvopc.cpp` | активен (07.2026) | [github](https://github.com/buggins/coolreader) |
| koreader/crengine + KOReader | C++/Lua | GPL-2+/AGPL-3 | тот же импорт FB3, в составе KOReader | очень активен | [github](https://github.com/koreader/crengine) |
| crengine-ng | C++ | GPL-2+ | движок-библиотека, FB3 в списке форматов | активен, 2724 коммита | [gitlab](https://gitlab.com/coolreader-ng/crengine-ng) |
| DirtyFb3Converter | Java | GPL-3 | FB3→FB2 (с потерями) | 2026, мелкий | [github](https://github.com/alm-2000/DirtyFb3Converter) |
| FB3Tools | C# | GPL-3 | shell-расширение Windows (миниатюры/метаданные .fb3 в Проводнике) | мёртв (2018) | [github](https://github.com/evpobr/FB3Tools) |
| calibre-плагины «FB3 Input», «FB3 Metadata Reader» | Python | — | конвертация/метаданные в calibre | 2018, работают в calibre 9.x | [mobileread](https://www.mobileread.com/forums/showthread.php?t=295455), [metadata](https://www.mobileread.com/forums/showthread.php?t=295347) |

**Чего нет:** ни одной FB3-библиотеки на Python (PyPI), Rust (crates.io), C#/NuGet, Go, npm. Прямого FB3→EPUB конвертера тоже нет (путь: calibre-плагин или FB3→FB2→что угодно). Pandoc FB3 не знает.

### 2.2 Что важно взять из существующего кода

- **crengine `fb3fmt.cpp` + `lvopc.cpp`** — доказательство, что импорт FB3 в C++ мал: открыть ZIP → OPC-пакет → найти часть с content type `application/fb3-body+xml` → SAX-разбор в DOM с маппингом элементов, картинки через связи, обложка через thumbnail-связь. GPL: изучать, не копировать.
- **FB3Reader (LGPL)** — готовая «шпаргалка» маппинга FB3→HTML (`TagMapper`: poem/stanza→div, subtitle→h6, epigraph/annotation→blockquote, underline→u и т.д.), логика **постраничных книжных сносок** (`BookStyleNotes`, `MaxFootnoteHeight` с фолбэком в попап), конвертация физических единиц (эмпирика: EM=0.89 font-size, EX=0.57, 100px=1 дюйм). Важно: клиент не парсит OPC — потребляет серверный JSON (`fb3_2_json.pl`).
- **FB3::Validator (Perl)** — оракул корректности для тестов собственного парсера; FB3-Convert — генератор тестовых файлов из FB2/EPUB.

### 2.3 C++-обвязка для парсинга под Windows

- **OPC-контейнер:** козырь платформы — **нативный COM OPC API Windows** ([`IOpcFactory`/`IOpcPackage`, msopc.dll](https://learn.microsoft.com/en-us/windows/win32/opc/open-packaging-conventions-overview)) — ровно та модель «content types + relationships + parts», что нужна FB3; корректно переживёт и «Hardcore»-структуру. Альтернатива без COM: **libzip** (BSD) или **minizip-ng** (zlib) + свой OPC-слой (~200–300 строк, образец — `lvopc.cpp`).
- **XML:** **pugixml** (MIT) — лучший дефолт: быстрый in-memory DOM, отличный UTF-8/UTF-16, XPath-подмножество; body большой книги — единицы МБ, DOM-подход нормален. **libxml2** (MIT) — только если нужна честная XSD-валидация (именно её использует FB3::Validator). Платформенные варианты: XmlLite (стриминговый, в составе Windows). RapidXML — не рекомендуется (заброшен с 2009).

---

## 3. Рендеринг текста под Windows 10/11 (C++)

### 3.1 Слои DirectWrite и главный выбор

DirectWrite слоёный: сверху `IDWriteTextFormat`/`IDWriteTextLayout` (готовый layout), снизу `IDWriteTextAnalyzer` (итемизация, BiDi, шейпинг, точки переноса строк) + отрисовка glyph run’ов.

**`IDWriteTextLayout` даёт бесплатно:** итемизацию, BiDi, шейпинг, фолбэк шрифтов, перенос по словам, базовую выключку, per-range форматирование, **hit-testing** (выделение/каретка), inline-объекты. **Не умеет:** переносы слов (hyphenation), качественную разбивку строк, висячую пунктуацию, деление layout между страницами без пересоздания (текст layout’а неизменяем), буквицы.

**Кастомный layout на `IDWriteTextAnalyzer`** — путь книжного качества: `AnalyzeLineBreakpoints` (UAX#14) → шейпинг `GetGlyphs`/`GetGlyphPlacements` → **свой перенос строк** → выключка через конвейер `IDWriteTextAnalyzer1`: [`GetJustificationOpportunities`](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_1/nf-dwrite_1-idwritetextanalyzer1-getjustificationopportunities) → `JustifyGlyphAdvances` → `GetJustifiedGlyphs` (script-aware: пробелы для кириллицы/латиницы, кашида для арабского). Вы переписываете только верхний слой — шейпинг-движок остаётся системным. Эталонный скелет — сэмпл Microsoft **[CustomLayout («FlowLayout»)](https://github.com/Microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/DirectWrite/CustomLayout)**; для пути «стандартный layout + свой renderer» — сэмпл PadWrite.

Практический вердикт: **v1 можно собрать на `IDWriteTextLayout` по-параграфно** (быстро, без переносов), но целевое качество (переносы + ровная выключка + висячая пунктуация + буквицы + сноски внизу страницы) достижимо только на кастомном layout. Модель «страница = список (параграф, диапазон строк)» одинаково работает в обоих вариантах — движок можно подменить позже.

### 3.2 Переносы слов

В DirectWrite переносов **нет ни на каком уровне**; ICU их тоже не делает (BreakIterator — только точки *возможного* перелома по UAX#14). Индустриальный стандарт — **[hunspell/hyphen](https://github.com/hunspell/hyphen)** (C, алгоритм Ляна из TeX; используется LibreOffice/Scribus, исторически Firefox) со словарями:
- русский: `hyph_ru_RU.dic` из TeX-паттернов ruhyphal ([gsnoff/hyphen-ru](https://github.com/gsnoff/hyphen-ru), [CTAN ruhyphen](https://ctan.org/tex-archive/language/hyphenation/ruhyphen)) — русский переносится паттернами отлично, и выключенный русский текст без переносов выглядит плохо (длинные слова → дыры);
- английский: `hyph_en_US.dic` из того же семейства словарей LibreOffice; общий индекс паттернов — [hyphenation.org](https://www.hyphenation.org/).

Chrome решил ту же задачу теми же TeX-паттернами (через движок Minikin/AOSP). Интеграция: при заполнении строки слово-переполнитель прогоняется через hyphen, лучшая точка переноса вставляется как дополнительная break opportunity с дошейпленным дефисом.

### 3.3 Разбивка строк: greedy vs Кнут-Пласс

Все системные стеки (DWrite layout, SkParagraph, браузеры) используют жадный алгоритм. Книжное качество даёт **Кнут-Пласс** (минимизация штрафов по всему абзацу; [обзор](https://en.wikipedia.org/wiki/Knuth%E2%80%93Plass_line-breaking_algorithm), [разбор Litherum](http://litherum.blogspot.com/2015/07/knuth-plass-line-breaking-algorithm.html)). Готовой поддерживаемой C++-библиотеки нет — но алгоритм ~300 строк поверх списка box/glue/penalty: boxes = кластеры шейпинга, glue = растяжимые пробелы из `GetJustificationOpportunities`, penalties = точки переносов из hyphen. Абзацы короткие — стоимость незаметна. План: v1 greedy+переносы, v2 Кнут-Пласс.

### 3.4 Режимы растеризации и DPI

- Рекомендация для читалки: **`DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC` + grayscale AA + субпиксельное позиционирование** (дробные advances — критично для ровной выключки). Классический субпиксельный ClearType умирает: ломается на OLED/PenTile, поворотах, дробном скейлинге, композиции; современный UI Microsoft по умолчанию grayscale. ClearType оставить опцией для 96-DPI ЖК. GDI-совместимые режимы не использовать никогда (целопиксельные advances портят выключку).
- Все расчёты в DIP, манифест **Per-Monitor DPI v2**, на `WM_DPICHANGED` — перерастеризация без перелайаута (геометрия страниц в DIP).
- Цветные шрифты/эмодзи: `TranslateColorGlyphRun` (COLR/CPAL/SVG/CBDT; позиционирование по монохромным метрикам — на layout не влияет).
- Вариативные шрифты: полная поддержка с Win10 1709 (`IDWriteTextLayout4::SetFontAxisValues`; ось `opsz` полезна для оптического размера основного текста vs заголовков). OpenType-фичи: `IDWriteTypography` / массивы `DWRITE_FONT_FEATURE` в `GetGlyphs`.
- Фолбэк шрифтов: `IDWriteFontFallbackBuilder` — детерминированная цепочка «шрифт книги → кириллический serif → Segoe UI Emoji» (подключается и к стандартному layout через `SetFontFallback`).

### 3.5 Производительность и пагинация

Ориентиры **Петцольда** ([Pagination with DirectWrite](http://www.charlespetzold.com/blog/2013/10/Pagination-with-DirectWrite.html)): 6 654 `IDWriteTextLayout` (по абзацу на роман Троллопа) создаются за **<0,5 с**; полная пагинация книги — **~0,6 с** после отсечения невидимой отрисовки; ре-пагинация — асинхронно с чередованием с рендерингом.

Рабочая схема:
- **Абзац — единица шейпинга/лайаута; страница — список ссылок (абзац, диапазон строк).**
- **Ленивая пагинация от якоря**: якорь чтения — позиция в тексте (не номер страницы); вперёд от якоря раскладываем сразу, остальное — фоновым потоком; номера страниц всей книги — лениво.
- Смена кегля: сохранить якорь = первый видимый символ, сбросить кэши, разложить только текущую страницу, остальное в фоне.
- Память: текст — неизменяемые UTF-16 буферы по главам; layout’ы — только окно вокруг позиции чтения (пересоздание дешёвое, <0,1 мс); битмапы страниц — текущая ±1–2.

### 3.6 Презентация: DirectComposition

Каждая страница рендерится в свою composition-поверхность/визуал; **перелистывание = анимация трансформов визуалов на потоке композитора** (60–120 Гц без пере-рендеринга). Кэш 3–5 страниц (текущая ± соседи). Для режима непрерывной прокрутки — flip-model swapchain со scrolled areas/dirty rects ([DXGI 1.2](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-1-2-presentation-improvements)). Варианты хоста: чистый Win32 + DirectComposition (самый лёгкий) или WinUI 3 (`SwapChainPanel` / [interop с Microsoft.UI.Composition](https://learn.microsoft.com/en-us/windows/apps/develop/composition/composition-native-interop)). Для гигантского виртуализованного контента существует `VirtualSurfaceImageSource` (колбэки по регионам — исторически именно для просмотрщиков документов).

### 3.7 Альтернативные стеки (когда и зачем)

| Стек | Вердикт |
|---|---|
| **DWriteCore** (WinAppSDK) | Тот же API, но идёт с приложением (новые фичи вплоть до Win10 1809). Минус: пока **нет интеропа с Direct2D** (рендер через `IDWriteBitmapRenderTarget`/glyph-атлас). Брать, только если нужна независимость от версии ОС или атлас-рендерер. Системный dwrite_3 на Win10/11 достаточен. |
| **Skia + SkParagraph** (HarfBuzz+ICU) | Google-класс шейпинга и одинаковый вывод кроссплатформенно, но тяжёлая зависимость, неродная растеризация и **переносов всё равно нет** (в Blink они выше Skia). Для Windows-only не оправдан. |
| **HarfBuzz + DirectWrite-растеризация** | путь Firefox; оправдан только при будущей кроссплатформенности или полном контроле шейпинга. `IDWriteTextAnalyzer` даёт тот же класс шейпинга без зависимостей. |
| **RichEditBox / Windows.UI.Text** | нежизнеспособно: контрол для редактирования, давится большими документами, нет пагинации/переносов. |
| **Rust-референсы** (cosmic-text, parley) | полезны как образцы чистого пайплайна itemize→shape→break→align; показательно: переносов и выключки нет и там — «книжный» H&J-слой везде пишут сами. |

Полезные вещи «книжного» качества, которые даёт только кастомный layout: **висячая пунктуация** (для русских «кавычек-ёлочек» ~50% выноса; ни один системный стек не умеет — сильный дифференциатор), буквицы (отдельный крупный glyph run + уменьшение ширины первых N строк), контроль висячих строк (widow/orphan), вертикальная выключка страницы.

---

## 4. Уроки архитектур существующих читалок

### 4.1 crengine (CoolReader/KOReader/crengine-ng) — главный образец

- **tinyDOM**: компактный DOM с 32-битными хэндлами узлов (~8–16 байт на узел в RAM), данные — в чанк-хранилищах (текст/элементы/rect’ы/стили), сжимаемых и выгружаемых в кэш-файл → книги больше RAM работали на 32 МБ e-ink устройств.
- **Layout один раз → нарезка на страницы** отдельным проходом (`lvpagesplitter`) с правилами page-break → перелистывание O(1).
- **Кэш-файл** хранит сериализованный DOM + вычисленные стили + rect’ы + список страниц → повторное открытие многомегабайтной книги мгновенно (ключ = документ + хэш настроек).
- **Позиции-xpointer’ы**: закладки/выделения/прогресс = `(узел исходного DOM, смещение)` вида `/FictionBook/body/section[3]/p[5]/text().25` — не зависят от кегля/полей/версии движка. Инженерия стабильности: версионирование формата (`gDOMVersionCurrent`, toStringV1/V2), позиции сериализуются **против исходной структуры документа, никогда — против внутренних layout-обёрток**. Это стоит скопировать с первого дня.
- crengine-ng — единственный open-source движок с заявленной поддержкой FB3.

### 4.2 KOReader

Граница «движок ↔ приложение» проведена по **семантике документа** (~30 методов: renderPage, getTextFromPositions, позиции…), а не по примитивам рисования — десять лет позволяла менять движки. Аннотации — **сайдкар-файл** рядом с книгой (`pos0`/`pos1`-xpointer’ы + извлечённый текст + стиль); прямоугольники подсветок пересчитываются при рендере из xpointer’ов → всегда корректны после reflow. Многофайловые EPUB-спайны объединяются в один DOM синтетическими обёртками `DocFragment[n]` — тот же приём годится для тел FB3.

### 4.3 FBReader

Модель «плоский поток абзацев» (не DOM): проще и дешевле по памяти, работала на КПК 2005 года, но плохо тянет вложенные блоки (таблицы, флоаты) — а они в FB3 есть. Позиции-тройки `(абзац, элемент, символ)` — более слабая валюта закладок, чем xpointer’ы. Кодовая база мертва (архив 2021, продукт ушёл в проприетарь).

### 4.4 Веб-путь: Readium/Thorium, calibre, WebView2

Пагинация HTML в веб-читалках — **CSS multi-columns** («страница» = колонка, перелистывание = сдвиг). Подтверждённые боли: полный layout всего документа при каждой смене настроек (однофайловое тело сконвертированного FB2/FB3 — многосекундный reflow), **структурно слабая стабильность позиций** (CFI/Locator-костыли: связка из нескольких избыточных локаторов + fuzzy-поиск по текстовому контексту; синтетические «позиции» по 1024 символа), невозможность честных постраничных сносок и точного контроля висячих строк, вес и IPC WebView2. Работоспособность пути доказана (Readest: Tauri+WebView2+foliate-js, 21k★), и **все permissive-компоненты именно здесь**: foliate-js (MIT, включая FB2→HTML конвертер), Readium CSS (BSD-3). calibre-вьюер прежде всего показывает приём: **нормализовать вход конвертером до рендера** — поддержка форматов живёт в конвертерах, не в рендерере.

### 4.5 SumatraPDF и MuPDF

`HtmlFormatter` Sumatra — красивый минимальный образец «стриминговый layout → display list (`DrawInstr`) → страницы» (несколько kLOC), но автор в итоге сдался и перевёл EPUB на движок MuPDF. MuPDF (`html-layout.c`, есть нативный FB2-парсер) — чистейший маленький box-model движок на C, **но AGPL** (коммерческое использование = лицензия Artifex). Мучения Sumatra с измерением текста в GDI+ — аргумент за DirectWrite-first.

### 4.6 Лицензии: что можно линковать, что только читать

| Можно линковать в закрытое приложение | Только изучать |
|---|---|
| Readium CSS (BSD-3), foliate-js (MIT), WebView2, pugixml (MIT), libzip (BSD), hunspell/hyphen (LGPL/MPL — уточнить редакцию), Windows OPC API, FB3Reader (LGPL — с оговорками динамической линковки/JS) | crengine/CoolReader/crengine-ng (GPL-2+), KOReader (AGPL-3), FBReader (GPL-2), SumatraPDF (GPL-3), calibre (GPL-3), MuPDF (AGPL-3) |

Кастомный движок должен быть **clean-room**: вдохновлённым crengine, но не скопированным.

---

## 5. Рекомендуемая архитектура (C++/WinRT)

**Пайплайн: OPC-парсер → компактный DOM → стили → блочный layout → строчный layout (DirectWrite) → нарезка на страницы → display list → Direct2D → DirectComposition.**

1. **Парсинг FB3**: Windows OPC API (`IOpcFactory`) или libzip+pugixml со своим OPC-слоем. Навигация строго по типам связей (`…/relationships/Book` → `…/body` → `…/image`), без хардкода путей. Тесты — на трёх официальных примерах (обязательно nightmare + Hardcore); оракул корректности — FB3::Validator.
2. **Документная модель**: дерево в духе tinyDOM (компактные узлы, строки в общих буферах), FB3-словарь напрямую (он мал — не нужна общая HTML-модель). Многочастные тела при необходимости — в один DOM через синтетические обёртки.
3. **Позиции**: xpointer-стиль против исходной структуры FB3 (`/fb3-body/section[uuid]/p[5]/text().25` — у секций уже есть UUID, это даже прочнее crengine); версия формата позиций — с первого дня. Аннотации — сайдкар/БД: пара позиций + извлечённый текст + стиль.
4. **Строчный layout**: кастомный, на `IDWriteTextAnalyzer(1)` (старт — от сэмпла CustomLayout): itemize → breakpoints → шейпинг → свой перенос строк (greedy + hunspell/hyphen c hyph_ru_RU/hyph_en_US; апгрейд до Кнута-Пласса) → `JustifyGlyphAdvances`. Это открывает: настоящую выключку с переносами, висячую пунктуацию, буквицы, **сноски внизу страницы** (FB3 их явно типизирует — жирный дифференциатор, образец логики — FB3Reader), widow/orphan-контроль, деление абзаца между страницами задаром (вы владеете списком строк). Фолбэк-план для v1 — по-абзацные `IDWriteTextLayout` + пагинация по Петцольду, с той же моделью страницы.
5. **Пагинация**: layout от якоря чтения, фоновый поток, ленивые номера страниц; кэш-файл crengine-стиля (сериализованный DOM + layout + список страниц, ключ = книга + хэш настроек) → мгновенное переоткрытие.
6. **Рендеринг**: dwrite_3 + Direct2D `DrawGlyphRun` в поверхности страниц; NATURAL_SYMMETRIC + grayscale + субпиксельное позиционирование; ClearType — опция; эмодзи через `TranslateColorGlyphRun`; Per-Monitor DPI v2.
7. **Презентация**: DirectComposition (или WinUI 3 SwapChainPanel/Composition-interop): страницы-визуалы, перелистывание — компоузер-анимации над кэшем 3–5 страниц; прокрутка — flip-model со scrolled areas.
8. **Интерфейс движка** — KOReader-стиль `IDocument`/`ILocator` (движок отдаёт страницы/позиции/текст диапазонов, UI не знает про DirectWrite): дёшево страхует и возможный WebView2-бэкенд для быстрого v1, и будущие форматы (FB2 — обязательный второй формат: реального корпуса FB3 вне ЛитРес мало).

### Порядок работ (предложение)

1. OPC+XML парсер FB3 → DOM + метаданные + извлечение картинок (тесты на официальных примерах).
2. v1-рендер: по-абзацные IDWriteTextLayout, пагинация, перелистывание на DirectComposition, позиции-xpointer’ы.
3. Кастомный layout: переносы (hyphen+словари) + выключка (`IDWriteTextAnalyzer1`) + сноски внизу страницы.
4. Кэш-файл, фоновая пагинация, аннотации.
5. Полировка типографики: Кнут-Пласс, висячая пунктуация, буквицы, вариативные шрифты (opsz).

---

## Источники (основные)

Формат: [gribuser/FB3](https://github.com/gribuser/FB3) · [Examples](https://github.com/gribuser/FB3/tree/master/Examples) · [FB3-Convert (CPAN)](https://metacpan.org/dist/FB3-Convert) · [FB3 (CPAN)](https://metacpan.org/dist/FB3) · [Litres/FB3Reader](https://github.com/Litres/FB3Reader) · [Litres/FB3Editor](https://github.com/Litres/FB3Editor) · [Habr: FB2 и FB3 (МакЦентр)](https://habr.com/ru/companies/maccentre/articles/411755/) · [gorky.media о fb3](https://gorky.media/context/strannovatyj-fb3-otchet-federatsii-izdatelej-i-portret-pirata/) · [calibre bug #1808532](https://bugs.launchpad.net/bugs/1808532) · [Wikipedia: FictionBook](https://en.wikipedia.org/wiki/FictionBook)

Реализации: [buggins/coolreader (fb3fmt.cpp, lvopc.cpp)](https://github.com/buggins/coolreader) · [koreader/crengine](https://github.com/koreader/crengine) · [crengine-ng](https://gitlab.com/coolreader-ng/crengine-ng) · [DirtyFb3Converter](https://github.com/alm-2000/DirtyFb3Converter) · [calibre FB3 Input plugin](https://www.mobileread.com/forums/showthread.php?t=295455) · [Windows OPC API](https://learn.microsoft.com/en-us/windows/win32/opc/open-packaging-conventions-overview) · [pugixml](https://pugixml.org) · [libzip](https://libzip.org)

Рендеринг: [DirectWrite portal](https://learn.microsoft.com/en-us/windows/win32/directwrite/direct-write-portal) · [CustomLayout sample](https://github.com/Microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/DirectWrite/CustomLayout) · [IDWriteTextAnalyzer1 justification](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_1/nf-dwrite_1-idwritetextanalyzer1-getjustificationopportunities) · [Petzold: Pagination with DirectWrite](http://www.charlespetzold.com/blog/2013/10/Pagination-with-DirectWrite.html) · [hunspell/hyphen](https://github.com/hunspell/hyphen) · [hyphen-ru](https://github.com/gsnoff/hyphen-ru) · [DWriteCore overview](https://learn.microsoft.com/en-us/windows/win32/directwrite/dwritecore-overview) · [DXGI 1.2 presentation](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-1-2-presentation-improvements) · [Composition interop (WinUI 3)](https://learn.microsoft.com/en-us/windows/apps/develop/composition/composition-native-interop) · [Windows Terminal AtlasEngine](https://github.com/microsoft/terminal/pull/11623) · [State of Text Rendering 2024](https://behdad.org/text2024/) · [Knuth–Plass](https://en.wikipedia.org/wiki/Knuth%E2%80%93Plass_line-breaking_algorithm)

Архитектуры: [KOReader](https://github.com/koreader/koreader) · [FBReader (архив)](https://github.com/geometer/FBReader) · [Readium CSS: pagination](https://github.com/readium/css/blob/master/docs/CSS03-injection_and_pagination.md) · [Readium Locators](https://readium.org/technical/r2-locator-architecture/) · [Thorium Reader](https://github.com/edrlab/thorium-reader) · [foliate-js](https://github.com/johnfactotum/foliate-js) · [Readest](https://github.com/readest/readest) · [SumatraPDF ebook engine](https://deepwiki.com/sumatrapdfreader/sumatrapdf/3.5-ebook-handling) · [MuPDF license](https://mupdf.readthedocs.io/en/1.27.2/license.html) · [Уроки 15 лет SumatraPDF](https://blog.kowalczyk.info/article/2f72237a4230410a888acbfce3dc0864/lessons-learned-from-15-years-of-sumatrapdf-an-open-source-windows-app.html)
