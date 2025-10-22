#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "ImageFlowProcessor.h"

namespace fs = std::filesystem;

static std::vector<std::string> listFilesBasic(const std::string &folderPath)
{
    std::vector<std::string> ret;
    try
    {
        for (const auto &entry : fs::directory_iterator(folderPath))
        {
            if (entry.is_regular_file())
            {
                ret.push_back(folderPath + entry.path().filename().string());
            }
        }
    }
    catch (const fs::filesystem_error &ex)
    {
        std::cerr << "�ļ�ϵͳ����: " << ex.what() << std::endl;
    }
    return ret;
}

int main()
{
    ImageFlow::ProcessConfig config{
        800,
        600,
        "hue=h=30:s=1",
        "jpg"};

    ImageFlow::ImageFlowProcessor processor(config);

    auto imagePaths = listFilesBasic("C:\\Users\\XLC\\Desktop\\3\\");

    auto start = std::chrono::system_clock::now();
    if (!processor.processImages(
            imagePaths,
            "C:\\Users\\XLC\\Desktop\\2"))
    {
        std::cout << "ͼ�����ѳɹ���ɣ�" << std::endl;
    }
    else
    {
        std::cout << "ͼƬ����ʧ�ܣ�" << std::endl;
    }
    auto end = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    std::cout << "������ɣ���ʱ��" << elapsed.count() << " ��" << std::endl;

    return 0;
}
