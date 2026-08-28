module;

// Packaging API — это C и COM, стандартной библиотеки они с собой не несут,
// поэтому им место в глобальном фрагменте модуля: `import std;` ниже они не
// трогают, и порядок включений больше ничего не значит.
#include <windows.h>
#include <msopc.h>
#include <objbase.h>

module bukvitsa.fb3;

import std;
import wxl.text;

import :opc;

namespace bukvitsa::fb3 {
namespace {

/// Строка, которую вернул COM: освобождать её должен вызывающий, и забыть
/// об этом легко, поэтому она не хранится голым указателем нигде.
class CoString {
public:
    ~CoString() { if (value_) CoTaskMemFree(value_); }
    CoString() = default;
    CoString(const CoString&) = delete;
    CoString& operator=(const CoString&) = delete;

    LPWSTR* put() { return &value_; }
    std::wstring_view view() const { return value_ ? std::wstring_view(value_) : std::wstring_view(); }
    std::wstring str() const { return value_ ? std::wstring(value_) : std::wstring(); }

private:
    LPWSTR value_ = nullptr;
};

class BStr {
public:
    ~BStr() { if (value_) SysFreeString(value_); }
    BSTR* put() { return &value_; }
    std::wstring str() const { return value_ ? std::wstring(value_, SysStringLen(value_)) : std::wstring(); }

private:
    BSTR value_ = nullptr;
};

/// Простой владеющий указатель на COM-интерфейс. Своего хватает: тащить сюда
/// C++/WinRT ради одного шаблона незачем, а FB3 не должен зависеть от WinRT.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (p_) p_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }

    T** put() { return &p_; }
    void** putVoid() { return reinterpret_cast<void**>(&p_); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

void check(HRESULT hr, const char* what) {
    if (FAILED(hr))
        throw std::system_error(hr, std::system_category(), what);
}

/// COM нужен потоку, а не пакету: приложение обычно уже его подняло
/// (CompositionHost делает это первым делом), но консольный тест — нет.
/// Повторная инициализация в том же режиме безвредна, чужой режим — не наша
/// забота, лишь бы COM работал.
void ensureComInitialized() {
    static thread_local bool done = false;
    if (done) return;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        check(hr, "CoInitializeEx");
    done = true;
}

}  // namespace

struct OpcPackage::Impl {
    ComPtr<IOpcFactory> factory;
    ComPtr<IOpcPackage> package;
    ComPtr<IOpcPartSet> parts;

    /// Часть по её адресу: общий хвост всех разрешений связей.
    std::optional<PackagePart> partAt(IOpcPartUri* uri) const {
        BOOL exists = FALSE;
        if (FAILED(parts->PartExists(uri, &exists)) || !exists)
            return std::nullopt;

        ComPtr<IOpcPart> part;
        if (FAILED(parts->GetPart(uri, part.put())))
            return std::nullopt;

        CoString contentType;
        part->GetContentType(contentType.put());

        BStr name;
        uri->GetRawUri(name.put());

        // Пакет пишет Windows, а не мы, и обещать за неё правильный UTF-16
        // нечего: repaired() ставит U+FFFD там, где обещать было бы нельзя.
        // По спецификации это в любом случае ASCII.
        return PackagePart{name.str(), wxl::text::repaired(contentType.view()).to_utf8()};
    }

    /// Часть, на которую ведёт связь: цель связи задана относительно её
    /// источника, поэтому адрес всегда собирается от источника, а не от корня.
    std::optional<PackagePart> resolve(IOpcRelationship* relationship) const {
        ComPtr<IOpcUri> source;
        if (FAILED(relationship->GetSourceUri(source.put())))
            return std::nullopt;

        ComPtr<IUri> target;
        if (FAILED(relationship->GetTargetUri(target.put())))
            return std::nullopt;

        ComPtr<IOpcPartUri> combined;
        if (FAILED(source->CombinePartUri(target.get(), combined.put())))
            return std::nullopt;

        return partAt(combined.get());
    }

    /// Набор связей части либо самого пакета, когда part пуст.
    ComPtr<IOpcRelationshipSet> relationshipsOf(const PackagePart* part) const {
        ComPtr<IOpcRelationshipSet> set;

        if (part == nullptr) {
            package->GetRelationshipSet(set.put());
            return set;
        }

        ComPtr<IOpcPartUri> uri;
        if (FAILED(factory->CreatePartUri(part->name.c_str(), uri.put())))
            return set;

        ComPtr<IOpcPart> opcPart;
        if (FAILED(parts->GetPart(uri.get(), opcPart.put())))
            return set;

        opcPart->GetRelationshipSet(set.put());
        return set;
    }
};

OpcPackage::OpcPackage(const std::filesystem::path& path) : impl_(new Impl) {
    ensureComInitialized();

    check(CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER,
                           __uuidof(IOpcFactory), impl_->factory.putVoid()),
          "CoCreateInstance(OpcFactory)");

    ComPtr<IStream> stream;
    check(impl_->factory->CreateStreamOnFile(path.c_str(), OPC_STREAM_IO_READ, nullptr, 0, stream.put()),
          "OPC: файл не открывается");

    check(impl_->factory->ReadPackageFromStream(stream.get(), OPC_CACHE_ON_ACCESS, impl_->package.put()),
          "OPC: это не пакет OPC");

    check(impl_->package->GetPartSet(impl_->parts.put()), "OPC: у пакета нет частей");
}

OpcPackage::~OpcPackage() { delete impl_; }

OpcPackage::OpcPackage(OpcPackage&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

OpcPackage& OpcPackage::operator=(OpcPackage&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

std::optional<PackagePart> OpcPackage::partByPackageRelationship(std::wstring_view relationshipType) const {
    return partByRelationship(PackagePart{}, relationshipType);
}

std::optional<PackagePart> OpcPackage::partByRelationship(const PackagePart& source,
                                                          std::wstring_view relationshipType) const {
    ComPtr<IOpcRelationshipSet> set = impl_->relationshipsOf(source.name.empty() ? nullptr : &source);
    if (!set) return std::nullopt;

    ComPtr<IOpcRelationshipEnumerator> enumerator;
    if (FAILED(set->GetEnumeratorForType(std::wstring(relationshipType).c_str(), enumerator.put())))
        return std::nullopt;

    BOOL has = FALSE;
    while (SUCCEEDED(enumerator->MoveNext(&has)) && has) {
        ComPtr<IOpcRelationship> relationship;
        if (FAILED(enumerator->GetCurrent(relationship.put())))
            break;

        if (auto part = impl_->resolve(relationship.get()))
            return part;
    }

    return std::nullopt;
}

std::optional<PackagePart> OpcPackage::partByRelationshipId(const PackagePart& source,
                                                            wxl::text::u8_view relationshipId) const {
    ComPtr<IOpcRelationshipSet> set = impl_->relationshipsOf(source.name.empty() ? nullptr : &source);
    if (!set) return std::nullopt;

    ComPtr<IOpcRelationship> relationship;
    if (FAILED(set->GetRelationship(relationshipId.to_utf16().c_str(), relationship.put())))
        return std::nullopt;

    return impl_->resolve(relationship.get());
}

std::vector<std::pair<wxl::text::u8_text, PackagePart>> OpcPackage::relationshipsOfType(
    const PackagePart& source, std::wstring_view relationshipType) const {
    std::vector<std::pair<wxl::text::u8_text, PackagePart>> found;

    ComPtr<IOpcRelationshipSet> set = impl_->relationshipsOf(source.name.empty() ? nullptr : &source);
    if (!set) return found;

    ComPtr<IOpcRelationshipEnumerator> enumerator;
    if (FAILED(set->GetEnumeratorForType(std::wstring(relationshipType).c_str(), enumerator.put())))
        return found;

    BOOL has = FALSE;
    while (SUCCEEDED(enumerator->MoveNext(&has)) && has) {
        ComPtr<IOpcRelationship> relationship;
        if (FAILED(enumerator->GetCurrent(relationship.put())))
            break;

        CoString id;
        relationship->GetId(id.put());

        if (auto part = impl_->resolve(relationship.get()))
            found.emplace_back(wxl::text::repaired(id.view()).to_utf8(), std::move(*part));
    }

    return found;
}

std::string OpcPackage::readPart(const PackagePart& part) const {
    ComPtr<IOpcPartUri> uri;
    check(impl_->factory->CreatePartUri(part.name.c_str(), uri.put()), "OPC: неверное имя части");

    ComPtr<IOpcPart> opcPart;
    check(impl_->parts->GetPart(uri.get(), opcPart.put()), "OPC: часть не найдена");

    ComPtr<IStream> stream;
    check(opcPart->GetContentStream(stream.put()), "OPC: часть не читается");

    std::string content;

    // Размер известен заранее, но полагаться на него нельзя: часть в пакете
    // сжата, и Read всё равно отдаёт столько, сколько распаковал за раз.
    STATSTG stat{};
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0)
        content.reserve(static_cast<std::size_t>(stat.cbSize.QuadPart));

    char buffer[64 * 1024];
    for (;;) {
        ULONG read = 0;
        const HRESULT hr = stream->Read(buffer, sizeof(buffer), &read);
        if (FAILED(hr))
            check(hr, "OPC: часть не дочитана");
        if (read == 0)
            break;
        content.append(buffer, read);
    }

    return content;
}

}  // namespace bukvitsa::fb3
