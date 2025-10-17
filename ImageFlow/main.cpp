#include <iostream>
#include <string>

#include "FilterGraphPool.h"
#include "ImageFlowProcessor.h"

int main()
{
    ImageFlow::ProcessConfig config{
        800,
        600,
        "hue=h=30:s=1",
        "jpg"};

    ImageFlow::ImageFlowProcessor processor;

    if (!processor.processImage(
            "C:\\Users\\XLC\\Desktop\\456.png",
            "C:\\Users\\XLC\\Desktop\\789.jpg",
            config))
    {
        std::cout << "ͼ�����ѳɹ���ɣ�" << std::endl;
    }
    else
    {
        std::cout << "ͼƬ����ʧ�ܣ�" << std::endl;
    }

    return 0;
}