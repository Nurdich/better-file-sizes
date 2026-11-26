// Minimal stand-in for the parts of C++/WinRT's winrt/base.h that the mod
// uses (com_ptr, guid, guid_of). mingw-w64 toolchains often lack cppwinrt.
#pragma once
#include <unknwn.h>
#include <guiddef.h>
#include <cstring>
#include <utility>

namespace winrt {

template <typename T>
class com_ptr {
   public:
    com_ptr() noexcept = default;
    com_ptr(std::nullptr_t) noexcept {}
    com_ptr(const com_ptr& o) noexcept : m_ptr(o.m_ptr) { add_ref(); }
    com_ptr(com_ptr&& o) noexcept : m_ptr(std::exchange(o.m_ptr, nullptr)) {}
    ~com_ptr() noexcept { release(); }
    com_ptr& operator=(const com_ptr& o) noexcept {
        if (this != &o) {
            o.add_ref();
            release();
            m_ptr = o.m_ptr;
        }
        return *this;
    }
    com_ptr& operator=(com_ptr&& o) noexcept {
        if (this != &o) {
            release();
            m_ptr = std::exchange(o.m_ptr, nullptr);
        }
        return *this;
    }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }
    T* operator->() const noexcept { return m_ptr; }
    T& operator*() const noexcept { return *m_ptr; }
    T* get() const noexcept { return m_ptr; }
    T** put() noexcept {
        release();
        return &m_ptr;
    }
    void** put_void() noexcept { return reinterpret_cast<void**>(put()); }
    void attach(T* p) noexcept {
        release();
        m_ptr = p;
    }
    T* detach() noexcept { return std::exchange(m_ptr, nullptr); }
    void copy_from(T* p) noexcept {
        if (p) p->AddRef();
        release();
        m_ptr = p;
    }
    template <typename U>
    com_ptr<U> try_as() const noexcept {
        com_ptr<U> r;
        if (m_ptr) m_ptr->QueryInterface(__uuidof(U), r.put_void());
        return r;
    }

   private:
    void add_ref() const noexcept {
        if (m_ptr) m_ptr->AddRef();
    }
    void release() noexcept {
        if (T* p = std::exchange(m_ptr, nullptr)) p->Release();
    }
    T* m_ptr = nullptr;
};

struct guid {
    GUID value{};
    guid() noexcept = default;
    guid(const GUID& g) noexcept : value(g) {}
    operator GUID() const noexcept { return value; }
    friend bool operator==(const guid& a, const guid& b) noexcept {
        return std::memcmp(&a.value, &b.value, sizeof(GUID)) == 0;
    }
    friend bool operator!=(const guid& a, const guid& b) noexcept {
        return !(a == b);
    }
};

template <typename T>
inline guid guid_of() noexcept {
    return guid{__uuidof(T)};
}

}  // namespace winrt
