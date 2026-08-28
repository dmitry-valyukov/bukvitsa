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

/// Файл книги, уже прочитанный кем-то другим, — как поток для Packaging API.
///
/// Нужен потому, что читать файл здесь нельзя: чтение с диска в этой читалке
/// живёт на рабочем потоке и приезжает сюда байтами, а разбор идёт в
/// интерфейсном. `CreateStreamOnFile` открыл бы файл сам и прочитал бы его
/// синхронно — то есть ровно то, чего мы избегаем.
///
/// Только чтение и только то, что спрашивает `ReadPackageFromStream`: `Read`,
/// `Seek` и размер из `Stat`. Всё остальное честно отвечает `E_NOTIMPL` —
/// поток, который делает вид, что умеет писать, хуже того, который не умеет.
///
/// Байтами он не владеет: они принадлежат книге и живут дольше пакета,
/// потому что пакет читает части по требованию (`OPC_CACHE_ON_ACCESS`).
class MemoryStream final : public IStream {
public:
    explicit MemoryStream(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;

        if (iid == __uuidof(IUnknown) || iid == __uuidof(ISequentialStream) ||
            iid == __uuidof(IStream)) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }

        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }

    // ---- ISequentialStream ----

    HRESULT STDMETHODCALLTYPE Read(void* into, ULONG wanted, ULONG* got) override {
        const std::size_t left = bytes_.size() - position_;
        const ULONG taken = static_cast<ULONG>(std::min<std::size_t>(wanted, left));

        if (taken != 0) std::memcpy(into, bytes_.data() + position_, taken);

        position_ += taken;

        if (got) *got = taken;

        return taken == wanted ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

    // ---- IStream ----

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD from, ULARGE_INTEGER* out) override {
        std::int64_t base = 0;

        switch (from) {
            case STREAM_SEEK_SET: base = 0; break;
            case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
            case STREAM_SEEK_END: base = static_cast<std::int64_t>(bytes_.size()); break;
            default: return STG_E_INVALIDFUNCTION;
        }

        const std::int64_t wanted = base + move.QuadPart;

        // Позади начала — ошибка; за концом — законно и означает пустое
        // чтение, как у всякого потока.
        if (wanted < 0) return STG_E_INVALIDFUNCTION;

        position_ = static_cast<std::size_t>(
            std::min<std::int64_t>(wanted, static_cast<std::int64_t>(bytes_.size())));

        if (out) out->QuadPart = position_;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* out, DWORD flags) override {
        if (!out) return E_POINTER;

        *out = STATSTG{};
        out->type = STGTY_STREAM;
        out->cbSize.QuadPart = bytes_.size();

        if (flags != STATFLAG_NONAME) out->pwcsName = nullptr;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if (!out) return E_POINTER;

        MemoryStream* copy = new MemoryStream(bytes_);
        copy->position_ = position_;

        *out = copy;
        return S_OK;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
    ULONG references_ = 1;
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

    check(impl_->factory->ReadPackageFromStream(stream.get(), OPC_CACHE_ON_ACCESS,
                                                impl_->package.put()),
          "OPC: это не пакет OPC");

    check(impl_->package->GetPartSet(impl_->parts.put()), "OPC: у пакета нет частей");
}

OpcPackage::OpcPackage(std::span<const std::byte> bytes) : impl_(new Impl) {
    ensureComInitialized();

    check(CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER,
                           __uuidof(IOpcFactory), impl_->factory.putVoid()),
          "CoCreateInstance(OpcFactory)");

    // Считаем от единицы, и первым владельцем становится ComPtr: дальше
    // пакет добавит свою ссылку и будет держать поток столько, сколько
    // читает части.
    ComPtr<IStream> stream;
    *stream.put() = new MemoryStream(bytes);

    check(impl_->factory->ReadPackageFromStream(stream.get(), OPC_CACHE_ON_ACCESS,
                                                impl_->package.put()),
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
