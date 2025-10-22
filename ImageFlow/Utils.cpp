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
 * @brief ��Windows�Ͻ��ַ����ӱ��ر���ת��ΪUTF-8��
 * @param [in] str ���ر���������ַ���
 * @return UTF-8�����ת���ַ�����
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
        std::cerr << "�޷����ַ����ӱ��ر���ת��ΪUTF-8��" << ec.what() << std::endl;
        return {};
    }
}
#else
/**
 * @brief �ڷ�Windowsƽ̨�ϣ����Ǽ��豾�ر�����UTF-8��
 * @param [in] str ���ر���������ַ���
 * @return �����ַ������ٶ�����UTF-8���롣
 */
std::string_view Utils::localToUtf8(std::string_view str)
{
    return str;
}
#endif
