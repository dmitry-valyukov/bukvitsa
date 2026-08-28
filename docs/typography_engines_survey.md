# Готовые движки типографики для Абажура: обзор и выводы

Дата: 22.08.2026. Вопрос: есть ли готовые открытые движки (желательно именно для читалок), чтобы не писать библиотеку Typography с нуля?

## Короткий ответ

Полный набор «переносы + выключка + пагинация книжного качества» в виде готовой встраиваемой библиотеки существует **ровно в одном C++-проекте — crengine-ng** (GPL-2+). Всё остальное — либо движки абзаца без переносов и пагинации (Pango, SkParagraph, Qt, litehtml, parley, cosmic-text), либо пакетные вёрстки, которые нельзя встроить в интерактивное приложение (SILE, Typst), либо веб-путь (WebView2 + foliate-js). При этом «свой» движок на DirectWrite — это на ~90% сборка из готовых компонентов, а не исследование: единственный кусок, которого нет ни в одной библиотеке, — алгоритм разбивки абзаца (Кнут-Пласс), ~500 строк по готовым референсам.

## 1. Движки именно для читалок

### crengine-ng — единственный настоящий кандидат «взять и встроить»
[gitlab.com/coolreader-ng/crengine-ng](https://gitlab.com/coolreader-ng/crengine-ng) · GPL-2.0+ · C++ · активен (коммиты июль 2026, релиз 0.9.13 ноябрь 2025, ~1–2 релиза в год, один мейнтейнер)

Что даёт из коробки — по сути всю нашу схему «FB3 → DOM → layout → страницы»:
- **FB2 и FB3 нативно** (`fb3fmt.cpp` + собственный OPC-слой, встроенный `fb3.css`), плюс EPUB/RTF/DOCX/MOBI и др.;
- **переносы** по TeX-паттернам, словари в комплекте, включая `hyph-ru-ru` и комбинированный **`hyph-ru-ru,en-us`** (ровно наш случай русской книги с англ. вкраплениями);
- выключка, разбивка на страницы (`lvpagesplitter`), кэш layout'а (мгновенное переоткрытие), xpointer-позиции;
- **сноски внизу страницы** (`PROP_FOOTNOTES_MODE=page_bottom`) — этого нет ни у одного другого кандидата;
- HiDPI через `PROP_RENDER_DPI`, субпиксельное сглаживание (RGB/BGR/V-RGB/Pentile), частично цветные эмодзи (CBDT);
- сборка CMake с явной поддержкой MSVC (майский коммит 2026 чинит именно Windows-сборку); зависимости (FreeType, HarfBuzz, FriBiDi, libunibreak и пр.) есть в vcpkg.

Интеграция: класс `LVDocView` (LoadDocument → Resize → getPageCount/goToPage → Draw в `LVColorDrawBuf` 32bpp, есть конструктор поверх чужого буфера) → загрузить страницу в `ID2D1Bitmap` и показать. Референс использования — фронтенды crqt-ng/crwx-ng.

Минусы: рендеринг **FreeType, не DirectWrite** — текст выглядит «по-кулридерски», не как нативный Windows-текст (гамма/хинтинг отличаются); вариативные шрифты почти нет (`font-variation-settings` — TODO в коде); COLRv1-эмодзи нет; документации нет (читать заголовки и фронтенды); проект держится на одном человеке; GPL-2 заражает приложение при распространении (для личного проекта — не проблема; публиковать придётся под GPL).

Прочие форки crengine: koreader/crengine — типографически самый продвинутый и самый живой (коммиты буквально вчера), но живёт как сабмодуль KOReader с Lua-биндингом, без стабильного C++ API; buggins/coolreader — оригинал, медленнее. Для встраивания правилен crengine-ng; фиксы можно черри-пикать между форками (общая родословная, общая лицензия).

### MuPDF — тёмная лошадка, стала сильнее
[mupdf.com](https://mupdf.com/) · **AGPL-3** (или платная коммерческая) · C · очень активен (1.28.2 август 2026)

Новое и важное: **с версии 1.27 (дек. 2025) в MuPDF появились переносы** — TeX-паттерны, `hyphens:auto` по `lang`, **русский словарь в базовой поставке**. Выключка есть. FB2 — первоклассный формат (свой парсер + fb2.css). Самый чистый C API из всех: `fz_open_document → fz_layout_document(w,h,em) → render в fz_pixmap` → D2D. MSVC-проекты в комплекте.

Минусы: FB3 нет (писать handler по образцу fb2 или конвертировать), сносок внизу страницы нет (остаются ссылками), CSS-подмножество, AGPL при распространении. Для личного проекта AGPL не мешает.

### Веб-путь: WebView2 + foliate-js
[foliate-js](https://github.com/johnfactotum/foliate-js) (MIT, активен, FB2 из коробки) + Chromium-рендеринг. Переносы `hyphens:auto` в WebView2 **работают для русского** (Chromium возит словари `.hyb`, включая `hyph-ru.hyb`; нужен `lang="ru"` на контенте). Всё современное бесплатно: ClearType-качество, вариативные шрифты, эмодзи, дробный DPI. Архитектура доказана Readest (21k★, Tauri+WebView2+foliate-js). FB3-адаптер — дни работы (у foliate-js простой интерфейс «книги»). Минусы прежние: пагинация CSS-колонками с её краевыми эффектами, сноски только попапом, нестабильность позиций, процесс WebView2.

Не годятся: epub.js (заброшен), readium-sdk C++ (мёртв с 2016), Vivliostyle (AGPL, формattер печати — медленный на целых книгах), paged.js (то же), Plato (Rust, привязан к Kobo-фреймбуферу, но его `layout.rs` — отличный референс Кнута-Пласса), FBReader (движок заброшен, SDK коммерческий), PocketBook SDK (ядро закрыто), legado (Android/Kotlin).

## 2. Движки абзаца общего назначения — почему они не решают задачу

| Движок | Лицензия | Выключка | Переносы | Пагинация | Вердикт |
|---|---|---|---|---|---|
| Pango 1.58 (акт. 2026) | LGPL | есть (жадная) | **нет** (только мягкие U+00AD) | нет | активен, vcpkg, но книжного не добавляет |
| SkParagraph (Skia) | BSD | есть | **нет** (Flutter-запрос открыт с 2018, в 2026 добавили лишь отрисовку дефиса на U+00AD) | нет | тяжёлая GN-сборка, тянет Skia как рендерер |
| Qt QTextDocument | LGPL | есть (грубая) | нет | есть (примитивная, без вдов/сирот) | самый полный «из коробки», но потолок качества низкий |
| litehtml 0.10 (июнь 2026) | BSD | есть (грубоватая) | нет | нет | интересен как HTML/CSS-слой поверх **вашего** DirectWrite-контейнера (так работает справка Qt Creator) |
| Minikin (Android) | Apache | есть | **есть** (+ оптимальная разбивка!) | нет | не собирается вне Android; но его гипенатор + .hyb-паттерны портируемы (прецедент: Chromium) |
| parley 0.10 (июнь 2026) | Apache/MIT | есть | **нет** (даже U+00AD — открытый issue #704) | нет | Rust, C-FFI нет |
| cosmic-text 0.19 | MIT/Apache | есть | **нет** | нет | то же |
| SILE / Typst | MIT / Apache | Кнут-Пласс! | есть! | есть! | пакетные вёрстки; SILE сломан на Windows, Typst не отделяется от своего компилятора — только как референс |

Вывод: ни один общий движок не даёт переносов — а без переносов выключенный русский текст выглядит плохо. Это главный водораздел.

## 3. «Свой» движок = сборка из компонентов (~90% готово)

Если оставаться на DirectWrite (за это — свежий прецедент: Zed на Windows в 2025 выбрал именно DirectWrite, а не Rust-стек):

| Роль | Компонент | Лицензия | Объём клея |
|---|---|---|---|
| Итемизация, BiDi, UAX#14, шейпинг, фолбэк | **DirectWrite сам** (`AnalyzeLineBreakpoints` возвращает и `isSoftHyphen`) | ОС | скелет = сэмпл CustomLayout |
| Переносы | **hypher** (Rust от автора Typst: 48 языков **включая русский**, паттерны скомпилированы в автоматы, zero-dep, no_std) за C ABI ~100 строк; чисто-C альтернатива — hunspell/hyphen + словари LibreOffice | MIT/Apache · MPL | 100–200 строк |
| Разбивка абзаца | **свой Кнут-Пласс ~500 строк**, порт с `typst-layout/inline/linebreak.rs` (Apache, эталонная современная реализация с прунингом) или bramstein/typeset | ваш код | 400–800 строк |
| Распределение выключки | **встроено в DirectWrite**: `IDWriteTextAnalyzer1::GetJustificationOpportunities → JustifyGlyphAdvances → GetJustifiedGlyphs` (script-aware, включая кашиду) | ОС | 100–200 строк |
| Сегментация доп. | не нужна (опционально libunibreak 7.0 для будущей портируемости) | zlib | — |

Прецеденты «Rust-компонент внутри C++-приложения через C FFI» — мейнстрим 2026 (Chrome возит Fontations/Skrifa вместо FreeType, Firefox — ICU4X под C++ LineBreaker). Готового open-source сочетания «DirectWrite + TeX-переносы + Кнут-Пласс» не существует — наша связка будет новой, но это низкорисковый клей, не исследование.

## 4. Рекомендация

Три жизнеспособные стратегии, по возрастанию труда и качества потолка:

1. **crengine-ng как движок v0.** Дымовой тест на 1 день: собрать библиотеку + 200-строчный D2D-харнесс, открыть русский FB2/FB3 со словарём `hyph-ru-ru,en-us` и сносками page_bottom. Если картинка (FreeType-рендеринг) устраивает — получаем работающую читалку за 2–4 недели, и весь план Typography можно отложить. GPL для личного проекта не мешает.
2. **Гибрид (рекомендуемый путь):** v0 на crengine-ng, параллельно строить свою Typography на DirectWrite из компонентов §3 за интерфейсом `IDocument`/`IPaginator` — тогда «тёплая ламповая» полировка (нативный рендеринг, вариативные шрифты, висячая пунктуация, буквицы) приходит поэтапно, а читать книги можно с первого месяца.
3. **Сразу своя Typography** — если процесс написания движка и есть удовольствие проекта. Объём: ~2–4 тыс. строк клея + 500 строк Кнута-Пласса, скелет — сэмпл CustomLayout.

Веб-путь (WebView2+foliate-js) держим как запасной: он самый быстрый до «работает» и единственный с бесплатным современным рендерингом, но противоречит духу проекта (сноски попапом, чужая пагинация).

## Источники

crengine-ng: [GitLab](https://gitlab.com/coolreader-ng/crengine-ng) · [зеркало](https://github.com/CrazyCoder/crengine-ng) · [crqt-ng](https://gitlab.com/coolreader-ng/crqt-ng) · [Gentoo changelog](https://packages.gentoo.org/packages/app-text/crengine-ng/changelog) · [koreader/crengine](https://github.com/koreader/crengine) · [buggins/coolreader](https://github.com/buggins/coolreader)
MuPDF: [сайт](https://mupdf.com/) · [история релизов](https://mupdf.com/releases/history) · [CHANGES (переносы в 1.27)](https://fossies.org/linux/mupdf/CHANGES) · [лицензия](https://mupdf.readthedocs.io/en/latest/)
Веб-путь: [foliate-js](https://github.com/johnfactotum/foliate-js) · [Readest](https://github.com/readest/readest) · [словари переносов Chromium (hyph-ru.hyb)](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/hyphenation-patterns/hyb) · [Intent to Ship: hyphens on Windows](https://groups.google.com/a/chromium.org/g/blink-dev/c/sIRXYKA4jGE)
Движки абзаца: [Pango](https://github.com/GNOME/pango) · [SkParagraph](https://github.com/google/skia/blob/main/modules/skparagraph/include/ParagraphBuilder.h) · [Flutter hyphenation issue](https://github.com/flutter/flutter/issues/18443) · [QTextDocument](https://doc.qt.io/qt-6/qtextdocument.html) · [litehtml](https://github.com/litehtml/litehtml) · [qlitehtml в Qt Creator](https://doc.qt.io/qtcreator/qtassistant-attribution-litehtml.html) · [Minikin](https://android.googlesource.com/platform/frameworks/minikin/+/refs/heads/main) · [порт гипенатора Minikin в Blink](https://codereview.chromium.org/2149803004/) · [parley](https://github.com/linebender/parley) · [cosmic-text](https://github.com/pop-os/cosmic-text) · [SILE](https://github.com/sile-typesetter/sile) · [Typst](https://github.com/typst/typst)
Компоненты: [hypher](https://github.com/typst/hypher) · [hunspell/hyphen](https://github.com/hunspell/hyphen) · [typst linebreak.rs](https://github.com/typst/typst/blob/main/crates/typst-layout/src/inline/linebreak.rs) · [bramstein/typeset](https://github.com/bramstein/typeset/) · [libunibreak](https://github.com/adah1972/libunibreak) · [DWrite justification](https://learn.microsoft.com/en-us/windows/win32/DirectWrite/justification--kerning--and-spacing) · [сэмпл CustomLayout](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/DirectWrite/CustomLayout) · [Zed на Windows = DirectWrite](https://zed.dev/blog/windows-progress-report) · [Chrome → Fontations/Skrifa](https://developer.chrome.com/blog/memory-safety-fonts) · [Plato layout.rs](https://github.com/baskerville/plato)
