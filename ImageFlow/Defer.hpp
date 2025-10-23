#pragma once

#include <functional>

class Defer
{
public:
    Defer(std::function<void()> func) : func_(func) {}
    ~Defer()
    {
        if (func_)
            func_();
    }

    Defer(const Defer &) = delete;
    Defer &operator=(const Defer &) = delete;

private:
    std::function<void()> func_;
};

#define DEFER(code)        Defer CONCAT(defer_, __LINE__)([&]() { code; })
#define CONCAT(a, b)       CONCAT_INNER(a, b)
#define CONCAT_INNER(a, b) a##b
