// Голден-тесты переводчика: MathML на входе, точная TeX-строка на выходе.
// Примеры — то, что реально пишут в EPUB издатели и конвертеры (MathJax,
// LaTeXML): формула квадратного уравнения, пределы, матрица, текст.

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

import bukvitsa.mathml;
import wxl.core;
import wxl.text;

using namespace bukvitsa;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    std::printf("%s %.*s\n", condition ? "  ok  " : "FAILED", static_cast<int>(what.size()),
                what.data());
    if (!condition) ++failures;
}

std::string toUtf8(std::wstring_view text) {
    const std::optional<wxl::text::u16_view> checked = wxl::text::checked(text);
    if (!checked) return "<не UTF-16>";
    return std::string{checked->to_utf8().chars()};
}

std::string_view bytes(std::u8string_view text) {
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// Перевод и сравнение с ожидаемой строкой; расхождение печатает обе.
void golden(std::string_view what, std::u8string_view mathml, std::wstring_view expected,
            bool display = false) {
    const std::optional<wxl::text::u8_view> input = wxl::text::checked(bytes(mathml));
    if (!input) {
        check(false, what);
        return;
    }
    const std::optional<mathml::TexFormula> formula = mathml::toTex(*input);
    if (!formula) {
        std::printf("FAILED %.*s: перевод не удался\n", static_cast<int>(what.size()),
                    what.data());
        ++failures;
        return;
    }
    const bool matches = formula->tex == expected && formula->display == display;
    check(matches, what);
    if (!matches) {
        std::printf("       ожидалось: %s\n", toUtf8(expected).c_str());
        std::printf("       вышло:     %s%s\n", toUtf8(formula->tex).c_str(),
                    formula->display != display ? " (display не совпал)" : "");
    }
}

bool fails(std::u8string_view mathml) {
    const std::optional<wxl::text::u8_view> input = wxl::text::checked(bytes(mathml));
    if (!input) return false;
    return !mathml::toTex(*input).has_value();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wxl::core::sta_memory_pool pool;

    golden("E=mc^2",
           u8"<math xmlns=\"http://www.w3.org/1998/Math/MathML\">"
           u8"<mi>E</mi><mo>=</mo><mi>m</mi><msup><mi>c</mi><mn>2</mn></msup></math>",
           L"E=m{c}^{2}");

    golden("формула квадратного уравнения, display",
           u8"<math display=\"block\"><mi>x</mi><mo>=</mo><mfrac>"
           u8"<mrow><mo>−</mo><mi>b</mi><mo>±</mo><msqrt>"
           u8"<msup><mi>b</mi><mn>2</mn></msup><mo>−</mo><mn>4</mn><mi>a</mi><mi>c</mi>"
           u8"</msqrt></mrow><mrow><mn>2</mn><mi>a</mi></mrow></mfrac></math>",
           L"x=\\frac{-b\\pm \\sqrt{{b}^{2}-4ac}}{2a}", true);

    golden("сумма с пределами",
           u8"<math><munderover><mo>∑</mo><mrow><mi>k</mi><mo>=</mo><mn>1</mn></mrow>"
           u8"<mi>n</mi></munderover><msup><mi>k</mi><mn>2</mn></msup></math>",
           L"\\sum _{k=1}^{n}{k}^{2}");

    golden("интеграл с невидимым умножением",
           u8"<math><munderover><mo>∫</mo><mn>0</mn><mi>∞</mi></munderover>"
           u8"<msup><mi>e</mi><mrow><mo>−</mo><mi>x</mi></mrow></msup>"
           u8"<mo>⁢</mo><mi>d</mi><mi>x</mi></math>",
           L"\\int _{0}^{\\infty }{e}^{-x}dx");

    golden("предел",
           u8"<math><munder><mi>lim</mi><mrow><mi>n</mi><mo>→</mo><mi>∞</mi></mrow>"
           u8"</munder><mfrac><mn>1</mn><mi>n</mi></mfrac></math>",
           L"\\lim _{n\\rightarrow \\infty }\\frac{1}{n}");

    golden("матрица в скобках",
           u8"<math><mfenced open=\"(\" close=\")\"><mtable>"
           u8"<mtr><mtd><mn>1</mn></mtd><mtd><mn>0</mn></mtd></mtr>"
           u8"<mtr><mtd><mn>0</mn></mtd><mtd><mn>1</mn></mtd></mtr>"
           u8"</mtable></mfenced></math>",
           L"\\left(\\begin{matrix}1 & 0 \\\\ 0 & 1\\end{matrix}\\right)");

    golden("вектор и текст кириллицей",
           u8"<math><mover><mi>v</mi><mo>→</mo></mover><mo>=</mo>"
           u8"<mtext>скорость</mtext></math>",
           L"\\vec{v}=\\text{скорость}");

    golden("греческие буквы и mathvariant",
           u8"<math><mi>π</mi><mo>≠</mo><mi mathvariant=\"normal\">d</mi></math>",
           L"\\pi \\neq \\mathrm{d}");

    golden("корень степени n",
           u8"<math><mroot><mi>x</mi><mn>3</mn></mroot></math>",
           L"\\sqrt[3]{x}");

    golden("функция и msubsup",
           u8"<math><mi>sin</mi><mo>⁡</mo><msubsup><mi>x</mi><mn>1</mn><mn>2</mn>"
           u8"</msubsup></math>",
           L"\\sin {x}_{1}^{2}");

    golden("semantics: перевод по презентации, аннотация не течёт",
           u8"<math><semantics><mrow><mi>a</mi><mo>+</mo><mi>b</mi></mrow>"
           u8"<annotation encoding=\"application/x-tex\">\\frobnicate{a}{b}</annotation>"
           u8"</semantics></math>",
           L"a+b");

    golden("незнакомый элемент прозрачен",
           u8"<math><mfancy><mi>x</mi></mfancy></math>", L"x");

    golden("префикс пространства имён не мешает",
           u8"<m:math xmlns:m=\"http://www.w3.org/1998/Math/MathML\">"
           u8"<m:mi>y</m:mi></m:math>",
           L"y");

    check(fails(u8"<math><mrow>оборванный"), "битый XML отвечает nullopt");
    check(fails(u8"<p>формулы нет</p>"), "XML без math отвечает nullopt");

    {
        std::u8string bomb = u8"<math>";
        for (int i = 0; i < 200; ++i) bomb += u8"<mrow>";
        bomb += u8"<mi>x</mi>";
        for (int i = 0; i < 200; ++i) bomb += u8"</mrow>";
        bomb += u8"</math>";
        check(fails(bomb), "враждебная глубина отвечает nullopt");
    }

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
