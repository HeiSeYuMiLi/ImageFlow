#include <iostream>
#include <string>

#include "ImageFlowProcessor.h"

// ʹ��ʾ��
int main()
{
    ImageFlow::ImageFlowProcessor processor;

    bool success = processor.processImage(
        "C:\\Users\\XLC\\Desktop\\456.png",
        "C:\\Users\\XLC\\Desktop\\789.jpg",
        "hue=h=30:s=1",
        800,
        600,
        "jpg");

    if (success)
    {
        std::cout << "ͼ�����ѳɹ���ɣ�" << std::endl;
    }
    else
    {
        std::cout << "ͼƬ����ʧ�ܣ�" << std::endl;
    }

    return 0;
}