#include <windows.h>

#include <algorithm>

// Свой заголовок после всех стандартных: он ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "book_index.h"

namespace bukvitsa::reader {
namespace {

/// Строчными — средствами Windows, а не `std::towlower`.
///
/// `towlower` смотрит в текущую локаль C, а она по умолчанию «C», где кириллицы
/// нет вовсе: поиск «Прометей» перестал бы находить «прометей». `CharLowerBuffW`
/// знает Unicode целиком и от локали процесса не зависит.
std::wstring lowered(std::wstring_view text) {
    std::wstring copy{text};
    if (!copy.empty()) {
        ::CharLowerBuffW(copy.data(), static_cast<DWORD>(copy.size()));
    }
    return copy;
}

/// Позиция символа абзаца в книге. Таблица позиций может быть короче текста —
/// у блоков без текста её нет вовсе, — и тогда отвечает начало блока.
std::uint32_t offsetAt(const typography::Block& block, std::size_t index) {
    const std::vector<std::uint32_t>& offsets = block.paragraph.charOffsets;
    return index < offsets.size() ? offsets[index] : block.charOffset;
}

/// Кусок текста вокруг находки: немного до и побольше после.
std::wstring contextAround(std::wstring_view text, std::size_t at, std::size_t length) {
    constexpr std::size_t kBefore = 30;
    constexpr std::size_t kAfter = 70;

    const std::size_t from = at > kBefore ? at - kBefore : 0;
    const std::size_t to = std::min(text.size(), at + length + kAfter);

    std::wstring out;
    if (from > 0) out += L"…";
    out.append(text.substr(from, to - from));
    if (to < text.size()) out += L"…";
    return out;
}

}  // namespace

std::vector<ContentsEntry> contentsOf(std::span<const typography::Block> blocks) {
    std::vector<ContentsEntry> contents;

    for (const typography::Block& block : blocks) {
        if (block.kind != typography::BlockKind::Title) continue;
        if (block.paragraph.text.empty()) continue;

        contents.push_back({block.paragraph.text, block.level, block.charOffset});
    }

    return contents;
}

std::vector<SearchHit> searchBook(std::span<const typography::Block> blocks,
                                  std::wstring_view needle, std::size_t limit) {
    std::vector<SearchHit> hits;
    if (needle.empty() || limit == 0) return hits;

    const std::wstring wanted = lowered(needle);

    for (const typography::Block& block : blocks) {
        if (block.paragraph.text.empty()) continue;

        // Абзац приводится к строчным целиком и один раз: искать в нём будут
        // столько раз, сколько в нём находок, а копия всё равно нужна — регистр
        // менять в исходном тексте нельзя, из него берётся отрывок для показа.
        const std::wstring haystack = lowered(block.paragraph.text);

        std::size_t at = haystack.find(wanted);
        while (at != std::wstring::npos) {
            hits.push_back({contextAround(block.paragraph.text, at, wanted.size()),
                            offsetAt(block, at)});
            if (hits.size() >= limit) return hits;

            at = haystack.find(wanted, at + wanted.size());
        }
    }

    return hits;
}

std::wstring hintAt(std::span<const typography::Block> blocks, std::uint32_t charOffset) {
    constexpr std::size_t kWords = 60;

    // Последний блок, начинающийся не позже искомой позиции: блоки идут по
    // возрастанию, и это проверено тестом вёрстки.
    const typography::Block* found = nullptr;
    for (const typography::Block& block : blocks) {
        if (block.charOffset > charOffset) break;
        if (!block.paragraph.text.empty()) found = &block;
    }
    if (!found) return {};

    std::wstring hint = found->paragraph.text.substr(0, kWords);
    if (found->paragraph.text.size() > kWords) hint += L"…";
    return hint;
}

}  // namespace bukvitsa::reader
