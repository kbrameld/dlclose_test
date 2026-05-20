// plugin.cpp
#include <memory>
#include <iostream>
#include "base.hpp"

struct Derived : Base {
    static int counter;
    Derived() { counter++; }
    int identify() override { return counter; }
};

int Derived::counter = 0; // Standard global initialization

extern "C" std::shared_ptr<Base> make_obj() {
    auto obj = std::make_shared<Derived>();
    std::cout << "    [Plugin] Current static counter: " << obj->identify() << "\n";
    return obj;
}
