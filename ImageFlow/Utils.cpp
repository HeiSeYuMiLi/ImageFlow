#include "Utils.h"
//------------------
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

using namespace ImageFlow;

#ifdef _WIN32
#include <windows.h>
/**
 * @brief 在Windows上将字符串从本地编码转换为UTF-8。
 * @param [in] str 本地编码的输入字符串
 * @return UTF-8编码的转换字符串。
 */
std::string Utils::localToUtf8(std::string_view str)
{
    if (str.empty())
        return {};

    try
    {
        int size_needed = MultiByteToWideChar(CP_ACP, 0, str.data(), (int)str.size(), NULL, 0);
        if (size_needed <= 0)
            return {};

        std::wstring wstr(size_needed, 0);
        MultiByteToWideChar(CP_ACP, 0, str.data(), (int)str.size(), &wstr[0], size_needed);
        int utf8_size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
        if (utf8_size_needed <= 0)
            return {};

        std::string utf8_str(utf8_size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &utf8_str[0], utf8_size_needed, NULL, NULL);
        return utf8_str;
    }
    catch (const std::exception &ec)
    {
        std::cerr << "无法将字符串从本地编码转换为UTF-8：" << ec.what() << std::endl;
        return {};
    }
}
#else
/**
 * @brief 在非Windows平台上，我们假设本地编码是UTF-8。
 * @param [in] str 本地编码的输入字符串
 * @return 输入字符串，假定采用UTF-8编码。
 */
std::string_view Utils::localToUtf8(std::string_view str)
{
    return str;
}
#endif
