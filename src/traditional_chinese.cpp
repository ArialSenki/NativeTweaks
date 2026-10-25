#include "traditional_chinese.hpp"

#include <Windows.h>

#include <unordered_map>

namespace zht {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<const wchar_t*, std::wstring> g_cache{};

void replace_all(std::wstring& text, std::wstring_view from, std::wstring_view to) {
    for (std::size_t at = text.find(from); at != std::wstring::npos;
         at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}

} // namespace

std::wstring convert(std::wstring_view simplified) {
    if (simplified.empty() || simplified.size() > INT_MAX) return std::wstring(simplified);
    const int length = static_cast<int>(simplified.size());
    std::wstring text(simplified.size(), L'\0');
    const int written = LCMapStringEx(L"zh-CN", LCMAP_TRADITIONAL_CHINESE,
        simplified.data(), length, text.data(), length, nullptr, nullptr, 0);
    if (written != length) return std::wstring(simplified);

    // Character mapping leaves some ambiguous characters; fix the usual ones.
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'后' && (i == 0 || text[i - 1] != L'皇'))
            text[i] = L'後';                                   // 后 -> 後
        else if (text[i] == L'里' && i > 0 && (text[i - 1] == L'這' ||
                 text[i - 1] == L'那' || text[i - 1] == L'哪'))
            text[i] = L'裡';                                   // 這里 -> 這裡
    }
    replace_all(text, L"游戲", L"遊戲");           // 游戲 -> 遊戲
    replace_all(text, L"加載", L"載入");           // 加載 -> 載入
    replace_all(text, L"圣", L"聖");               // 圣杯瓶, 圣印记: LCMapStringEx keeps 圣
    return text;
}

const wchar_t* convert(const wchar_t* simplified) noexcept {
    if (!simplified) return simplified;
    AcquireSRWLockExclusive(&g_lock);
    const wchar_t* result = simplified;
    try {
        auto found = g_cache.find(simplified);
        if (found == g_cache.end())
            found = g_cache.emplace(simplified, convert(std::wstring_view(simplified))).first;
        result = found->second.c_str();
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

} // namespace zht
