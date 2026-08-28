module bukvitsa.fb3;

namespace bukvitsa::fb3 {
namespace {

/// Нагрузка принадлежит одному виду узла, и спрашивать её у другого —
/// ошибка вызывающего, а не повод вернуть чужую структуру.
template <typename T>
const T* payloadOf(NodeKind actual, NodeKind expected, const void* data) {
    return actual == expected ? static_cast<const T*>(data) : nullptr;
}

}  // namespace

const SectionData* Node::section() const { return payloadOf<SectionData>(kind_, NodeKind::Section, data_); }
const DivData* Node::div() const { return payloadOf<DivData>(kind_, NodeKind::Div, data_); }
const ImageData* Node::image() const { return payloadOf<ImageData>(kind_, NodeKind::Image, data_); }
const NoteRefData* Node::noteRef() const { return payloadOf<NoteRefData>(kind_, NodeKind::NoteRef, data_); }
const LinkData* Node::link() const { return payloadOf<LinkData>(kind_, NodeKind::Link, data_); }
const ListData* Node::list() const { return payloadOf<ListData>(kind_, NodeKind::List, data_); }
const SpanData* Node::span() const { return payloadOf<SpanData>(kind_, NodeKind::Span, data_); }

const TableCellData* Node::tableCell() const {
    return payloadOf<TableCellData>(kind_, NodeKind::TableCell, data_);
}

}  // namespace bukvitsa::fb3
