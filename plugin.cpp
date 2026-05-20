#include <memory>
#include "base.hpp"
struct Derived : Base { int identify() override { return 1234; } };
extern "C" std::shared_ptr<Base> make_obj() { return std::make_shared<Derived>(); }
