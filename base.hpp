#pragma once
struct Base { virtual ~Base() = default; virtual int identify() { return 42; } };
