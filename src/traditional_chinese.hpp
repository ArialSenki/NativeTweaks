#pragma once

#include <string>
#include <string_view>
#include <type_traits>

// Runtime Simplified -> Traditional Chinese conversion for the menu texts, so
// one DLL can offer both scripts (Windows LCMapStringEx plus a few fixes).
namespace zht {

std::wstring convert(std::wstring_view simplified);

// Converted copy of a literal, cached by address; the original on failure.
const wchar_t* convert(const wchar_t* simplified) noexcept;

// Converts every string of a text table made only of const wchar_t* fields.
template <class T>
T convert_table(const T& source) noexcept {
    static_assert(std::is_trivially_copyable_v<T> &&
        sizeof(T) % sizeof(const wchar_t*) == 0, "text table must hold only strings");
    T result = source;
    auto** fields = reinterpret_cast<const wchar_t**>(&result);
    for (std::size_t i = 0; i < sizeof(T) / sizeof(const wchar_t*); ++i)
        fields[i] = convert(fields[i]);
    return result;
}

} // namespace zht
