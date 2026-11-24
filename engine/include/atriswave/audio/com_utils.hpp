#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace atriswave::audio {

/// Lightweight, zero-dependency COM smart pointer.
template <typename T>
class ComPtr final {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}

    explicit ComPtr(T* ptr) noexcept : ptr_(ptr) {
        if (ptr_ != nullptr) {
            ptr_->AddRef();
        }
    }

    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        if (ptr_ != nullptr) {
            ptr_->AddRef();
        }
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

    ~ComPtr() noexcept { reset(); }

    ComPtr& operator=(const ComPtr& other) noexcept {
        if (this != &other) {
            ComPtr(other).swap(*this);
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }

    ComPtr& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    [[nodiscard]] T* get() const noexcept { return ptr_; }
    [[nodiscard]] T* operator->() const noexcept { return ptr_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

    [[nodiscard]] T** put() noexcept {
        reset();
        return &ptr_;
    }

    [[nodiscard]] void** put_void() noexcept {
        reset();
        return reinterpret_cast<void**>(&ptr_);
    }

    void attach(T* ptr) noexcept {
        reset();
        ptr_ = ptr;
    }

    T* detach() noexcept {
        return std::exchange(ptr_, nullptr);
    }

    void reset() noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    void swap(ComPtr& other) noexcept {
        std::swap(ptr_, other.ptr_);
    }

    template <typename U>
    HRESULT as(ComPtr<U>& out) const noexcept {
        if (ptr_ == nullptr) {
            return E_POINTER;
        }
        return ptr_->QueryInterface(__uuidof(U), out.put_void());
    }

private:
    T* ptr_{nullptr};
};

/// RAII COM initialization guard that safely handles RPC_E_CHANGED_MODE.
class ScopedComInit final {
public:
    explicit ScopedComInit(DWORD coinit = COINIT_MULTITHREADED) noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, coinit);
        if (hr == S_OK || hr == S_FALSE) {
            should_uninit_ = true;
            ok_ = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            // COM is already initialized on this thread with a different concurrency model.
            should_uninit_ = false;
            ok_ = true;
        }
    }

    ~ScopedComInit() noexcept {
        if (should_uninit_) {
            CoUninitialize();
        }
    }

    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool should_uninit_{false};
    bool ok_{false};
};

inline std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

inline std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

inline std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char ch : input) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                out += buf;
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

} // namespace atriswave::audio
