#include "pagination.h"

#include "main_loop.h"

import std;

namespace bukvitsa {

namespace {

/// Блоков между передышками. Меньше — отзывчивее окно, больше — меньше
/// накладных расходов на возобновления; здесь взято на глаз.
constexpr int blocksPerSlice = 20;

/// «Вёрстка» одного блока: работа, которой не жалко, лишь бы она стоила
/// времени и не оптимизировалась в ничто.
void measureBlock(const Layout& layout, int block)
{
    volatile double ink = 0;

    for (int i = 0; i < layout.fontSize * 400; ++i) ink = ink + (block % 7) * 0.5 + i % 3;
}

/// Пункт второй: показать страницу, на которой читатель остановился.
void showPage(const Pages& pages)
{
    std::cout << std::format("[страница {}]", pages.counted);
}

}  // namespace

io::task paginate(const Layout& layout, Pages& pages, io::cancellation_token token)
{
    for (int block = 0; block < layout.blocks; ++block) {
        measureBlock(layout, block);

        ++pages.blocksDone;
        if (pages.blocksDone % layout.blocksPerPage == 0) ++pages.counted;

        // Место чтения пройдено — можно показывать страницу. Перед показом
        // отмена проверяется отдельно: считали мы долго, и раскладка за это
        // время могла смениться.
        if (pages.blocksDone == layout.readingBlock) {
            token.throw_if_canceled();
            showPage(pages);
        }

        // Передышка. Один и тот же механизм по обе стороны от места чтения:
        // отдать поток насосу, а вернувшись — проверить, не отменили ли нас.
        if (pages.blocksDone % blocksPerSlice == 0) {
            co_await io::yield();
            token.throw_if_canceled();

            std::cout << '#';  // кусок пагинации — виден на ленте
        }
    }

    pages.wholeBook = true;
}

}  // namespace bukvitsa
