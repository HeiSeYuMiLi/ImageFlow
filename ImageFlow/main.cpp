#include <iostream>
#include <string>

#include "ImageFlowProcessor.h"

// 使用示例
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
        std::cout << "图像处理已成功完成！" << std::endl;
    }
    else
    {
        std::cout << "图片处理失败！" << std::endl;
    }

    return 0;
}