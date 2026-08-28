// Кнут-Пласс: динамическое программирование по местам возможного разрыва.
//
// Устройство ровно как в статье «Breaking Paragraphs into Lines» (1981).
// Активный узел — это «строка могла бы кончиться здесь»; для каждого нового
// места разрыва перебираются все активные узлы, из них выбирается тот, от
// которого сюда дешевле всего дойти, и он становится предком нового узла.
// Узлы, от которых уже нельзя дотянуться до текущего места, не сжав строку
// сильнее допустимого, из списка выбывают — иначе он рос бы квадратично.
//
// Отличие от статьи одно, и оно от практики: если абзац не разбивается при
// заданной терпимости (одно длинное слово шире полосы — в книгах бывает,
// особенно с URL), делается аварийный проход, где допустимо всё. Лучше строка
// с дырой, чем абзац, которого не видно.

#include <algorithm>
#include <cmath>

// Свой заголовок после всех стандартных: он ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/linebreak.h"

namespace bukvitsa::typography {
namespace {

/// Накопленные суммы до элемента: ширина строки между двумя переломами —
/// это разность сумм, а не отдельный проход по элементам.
struct Totals {
    float width = 0.0f;
    float stretch = 0.0f;
    float shrink = 0.0f;
};

/// Классы плотности строки. Соседние строки разных классов выглядят
/// по-разному набранными, и за это Кнут берёт отдельный штраф.
enum class Fitness : std::uint8_t { Tight, Decent, Loose, VeryLoose };

Fitness fitnessOf(float ratio) {
    if (ratio < -0.5f) return Fitness::Tight;
    if (ratio <= 0.5f) return Fitness::Decent;
    if (ratio <= 1.0f) return Fitness::Loose;
    return Fitness::VeryLoose;
}

bool farApart(Fitness a, Fitness b) {
    const int difference = static_cast<int>(a) - static_cast<int>(b);
    return difference > 1 || difference < -1;
}

struct Node {
    std::uint32_t position = 0;   ///< индекс элемента, на котором кончается строка
    std::uint32_t line = 0;       ///< сколько строк набрано до этого места
    Fitness fitness = Fitness::Decent;
    Totals totals;                ///< суммы сразу после этого перелома
    double demerits = 0.0;
    std::uint32_t previous = kNone;

    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
};

class Breaker {
public:
    Breaker(std::span<const BreakItem> items, std::span<const float> lineWidths,
            const BreakSettings& settings)
        : items_(items), lineWidths_(lineWidths), settings_(settings) {}

    /// Проходы от строгого к всё более снисходительному. Отступать приходится
    /// чаще, чем кажется: абзац в одну-две строки нередко не имеет ни одного
    /// решения в заданных рамках — набранный целиком, он чуть шире полосы, а
    /// разорванный надвое даёт строку, которую нечем растянуть. Кнут в таком
    /// случае печатает `Overfull \hbox` и оставляет разбираться человеку;
    /// читалке разбираться не с кем, поэтому она последовательно уступает —
    /// сначала в плотности, потом в свободе, и лишь в конце во всём сразу.
    std::vector<std::uint32_t> run() {
        if (items_.empty() || lineWidths_.empty())
            return {};

        const float relaxedShrink = -3.0f;

        // Как задумано.
        if (auto result = attempt(settings_.tolerance, settings_.minAdjustmentRatio, false);
            !result.empty())
            return result;

        // Плотнее, чем хотелось бы: строка сжата сильнее, чем обещали пробелы.
        // Из двух зол это меньшее — слишком плотная строка читается, слишком
        // растянутая рассыпается на слова.
        if (auto result = attempt(settings_.tolerance, relaxedShrink, false); !result.empty())
            return result;

        // Свободнее, чем хотелось бы.
        if (auto result = attempt(kInfinitePenalty, relaxedShrink, false); !result.empty())
            return result;

        // Со строкой, вылезающей за поле. Так бывает, когда слово, которое в
        // полосу помещается, не помещается в первую строку — она короче на
        // абзацный отступ. Разрывать слово тут неверно: оно прекрасно встанет
        // на следующей строке, — а верно позволить одной строке вылезти, как
        // делает Кнут, печатая `Overfull \hbox`.
        //
        // Разбивка при этом честно берёт ровно одну плохую строку там, где
        // иначе никак, а не разваливает из-за неё весь абзац: скверность
        // растёт кубом и не упирается ни в какой потолок, см. badness().
        if (auto result = attempt(kInfinitePenalty, -kInfinitePenalty, false); !result.empty())
            return result;

        // Аварийный проход: терпим любую строку, лишь бы абзац появился.
        return attempt(kInfinitePenalty, -kInfinitePenalty, true);
    }

private:
    std::span<const BreakItem> items_;
    std::span<const float> lineWidths_;
    BreakSettings settings_;

    std::vector<Node> nodes_;
    std::vector<std::uint32_t> active_;

    float lineWidth(std::uint32_t line) const {
        const std::size_t index = std::min<std::size_t>(line, lineWidths_.size() - 1);
        return lineWidths_[index];
    }

    /// Можно ли рвать перед этим элементом.
    bool isBreakpoint(std::size_t at) const {
        const BreakItem& item = items_[at];

        if (item.kind == BreakItem::Kind::Penalty)
            return item.penalty < kInfinitePenalty;

        // Клей — место разрыва только сразу после бокса: иначе разрыв попал бы
        // в середину пробельной последовательности.
        return item.kind == BreakItem::Kind::Glue && at > 0 &&
               items_[at - 1].kind == BreakItem::Kind::Box;
    }

    /// Ширина от перелома до текущего места. Клей, стоящий на самом переломе,
    /// в строку не входит: он и есть то, что съедено разрывом.
    static float naturalWidth(const Totals& from, const Totals& to) { return to.width - from.width; }

    /// Насколько строку придётся растянуть (>0) или сжать (<0), чтобы она
    /// заняла полосу. Бесконечность — когда тянуть нечем.
    float adjustmentRatio(const Node& node, const Totals& at, std::size_t breakAt) const {
        float width = naturalWidth(node.totals, at);

        // Штраф на переломе печатается (дефис переноса) и потому занимает место.
        if (items_[breakAt].kind == BreakItem::Kind::Penalty)
            width += items_[breakAt].width;

        const float available = lineWidth(node.line);

        if (width < available) {
            const float stretch = at.stretch - node.totals.stretch + settings_.lineEndStretch;
            return stretch > 0.0f ? (available - width) / stretch : kInfinitePenalty;
        }
        if (width > available) {
            const float shrink = at.shrink - node.totals.shrink;
            if (shrink > 0.0f)
                return (available - width) / shrink;

            // Сжимать нечем — строка вылезет за поле. Ответить здесь просто
            // «минус бесконечность» нельзя: тогда строка, вылезшая на волосок,
            // и строка, вылезшая вдесятеро, окажутся одинаково плохими, и
            // разбивка, которой всё равно придётся взять одну плохую, выберет
            // худшую. Поэтому коэффициент несёт величину переполнения.
            return -1.0f - 100.0f * (width - available) / std::max(available, 1.0f);
        }
        return 0.0f;
    }

    /// Скверность строки по Кнуту: сто кубов коэффициента подгонки. Куб — чтобы
    /// вдвое более растянутая строка была не вдвое, а вшестеро хуже: глаз
    /// замечает дыру нелинейно.
    ///
    /// Потолка нет, в отличие от TeX, и это принципиально. У Кнута `inf_bad`
    /// существует потому, что строка со скверностью выше порога просто
    /// отвергается: разбивка её никогда не выберет, и различать «плохо» и
    /// «чудовищно» незачем. Наши снисходительные проходы, наоборот, только
    /// такие строки и выбирают — им приходится, — и вся их работа держится
    /// ровно на этом различии. С потолком одна чудовищная строка стоит столько
    /// же, сколько чудовищная плюс десять хороших, и разбивка с чистой совестью
    /// кладёт весь абзац в одну строку.
    ///
    /// Считается в double: без потолка куб коэффициента легко выходит за float.
    static double badness(float ratio) {
        const double value = std::abs(static_cast<double>(ratio));
        return 100.0 * value * value * value;
    }

    double demeritsFor(const Node& from, std::size_t breakAt, float ratio, Fitness fitness) const {
        const BreakItem& item = items_[breakAt];
        const float penalty = item.kind == BreakItem::Kind::Penalty ? item.penalty : 0.0f;
        const double base = settings_.linePenalty + badness(ratio);

        double demerits;
        if (penalty >= 0.0f)
            demerits = (base + penalty) * (base + penalty);
        else if (penalty > -kInfinitePenalty)
            demerits = base * base - penalty * penalty;
        else
            demerits = base * base;

        if (item.flagged && from.position < items_.size() &&
            items_[from.position].kind == BreakItem::Kind::Penalty && items_[from.position].flagged)
            demerits += settings_.doubleHyphenDemerits;

        if (farApart(fitness, from.fitness))
            demerits += settings_.adjacentDemerits;

        return demerits + from.demerits;
    }

    /// Суммы после элемента: клей и растяжимость, стоящие сразу за переломом,
    /// в следующую строку не входят — их съедает разрыв.
    Totals totalsAfterBreak(std::size_t breakAt, Totals running) const {
        for (std::size_t at = breakAt; at < items_.size(); ++at) {
            const BreakItem& item = items_[at];
            if (item.kind == BreakItem::Kind::Box)
                break;
            if (item.kind == BreakItem::Kind::Penalty && at > breakAt)
                break;

            if (item.kind == BreakItem::Kind::Glue) {
                running.width += item.width;
                running.stretch += item.stretch;
                running.shrink += item.shrink;
            }
        }
        return running;
    }

    std::vector<std::uint32_t> attempt(float tolerance, float minRatio, bool desperate) {
        nodes_.clear();
        active_.clear();

        nodes_.push_back(Node{});
        active_.push_back(0);

        Totals running;
        std::vector<std::uint32_t> stillActive;
        std::vector<std::uint32_t> born;

        for (std::size_t at = 0; at < items_.size(); ++at) {
            if (isBreakpoint(at))
                considerBreak(at, running, tolerance, minRatio, desperate, stillActive, born);

            const BreakItem& item = items_[at];
            if (item.kind != BreakItem::Kind::Penalty) {
                running.width += item.width;
                running.stretch += item.stretch;
                running.shrink += item.shrink;
            }
        }

        return collect();
    }

    void considerBreak(std::size_t at, const Totals& running, float tolerance, float minRatio,
                       bool desperate,
                       std::vector<std::uint32_t>& stillActive, std::vector<std::uint32_t>& born) {
        const bool forced = items_[at].kind == BreakItem::Kind::Penalty &&
                            items_[at].penalty <= -kInfinitePenalty;

        stillActive.clear();
        born.clear();

        // Лучший предок для каждого класса плотности: узлы одной плотности
        // взаимозаменяемы, и хранить стоит только дешёвый.
        constexpr std::size_t kClasses = 4;
        double bestDemerits[kClasses];
        std::uint32_t bestFrom[kClasses];
        float bestRatio[kClasses];
        for (std::size_t i = 0; i < kClasses; ++i) {
            bestDemerits[i] = std::numeric_limits<double>::max();
            bestFrom[i] = Node::kNone;
            bestRatio[i] = 0.0f;
        }

        bool anyFeasible = false;

        for (const std::uint32_t index : active_) {
            const Node& node = nodes_[index];
            const float ratio = adjustmentRatio(node, running, at);

            // Узел, до которого уже не дотянуться: строка от него сюда не
            // помещается даже полностью сжатой. Дальше будет только хуже, так
            // что он выбывает — на этом и держится линейность алгоритма.
            // Обязательный разрыв закрывает все узлы разом.
            const bool tooTight = ratio < minRatio;
            if (!tooTight && !forced)
                stillActive.push_back(index);

            const bool feasible =
                desperate || (ratio >= minRatio && ratio <= tolerance);
            if (!feasible)
                continue;

            const float clamped = desperate ? std::clamp(ratio, -1.0f, 10.0f) : ratio;
            const Fitness fitness = fitnessOf(clamped);
            const double demerits = demeritsFor(node, at, clamped, fitness);
            const std::size_t klass = static_cast<std::size_t>(fitness);

            anyFeasible = true;
            if (demerits < bestDemerits[klass]) {
                bestDemerits[klass] = demerits;
                bestFrom[klass] = index;
                bestRatio[klass] = clamped;
            }
        }

        if (anyFeasible) {
            const Totals after = totalsAfterBreak(at, running);

            for (std::size_t klass = 0; klass < kClasses; ++klass) {
                if (bestFrom[klass] == Node::kNone) continue;

                Node child;
                child.position = static_cast<std::uint32_t>(at);
                child.line = nodes_[bestFrom[klass]].line + 1;
                child.fitness = static_cast<Fitness>(klass);
                child.totals = after;
                child.demerits = bestDemerits[klass];
                child.previous = bestFrom[klass];

                nodes_.push_back(child);
                born.push_back(static_cast<std::uint32_t>(nodes_.size() - 1));
            }
        }

        active_ = stillActive;
        active_.insert(active_.end(), born.begin(), born.end());
    }

    /// Дешевейший узел, стоящий на завершающем штрафе, и путь к нему.
    std::vector<std::uint32_t> collect() const {
        std::uint32_t best = Node::kNone;

        for (std::uint32_t index = 1; index < nodes_.size(); ++index) {
            const Node& node = nodes_[index];
            if (node.position + 1 != items_.size()) continue;
            if (best == Node::kNone || node.demerits < nodes_[best].demerits) best = index;
        }

        if (best == Node::kNone)
            return {};

        std::vector<std::uint32_t> breaks;
        for (std::uint32_t index = best; index != 0 && index != Node::kNone;
             index = nodes_[index].previous)
            breaks.push_back(nodes_[index].position);

        std::reverse(breaks.begin(), breaks.end());
        return breaks;
    }
};

}  // namespace

std::vector<std::uint32_t> breakLines(std::span<const BreakItem> items,
                                      std::span<const float> lineWidths,
                                      const BreakSettings& settings) {
    return Breaker{items, lineWidths, settings}.run();
}

}  // namespace bukvitsa::typography
