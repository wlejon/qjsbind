# qjsbind

A C++20 header-only binding library for [QuickJS](https://bellard.org/quickjs/), providing a fluent API to expose C++ classes and functions to JavaScript with minimal boilerplate.

## Features

- **Header-only** -- just include and go, no compilation step
- **Fluent builder API** -- register classes, methods, and properties with readable method chaining
- **Automatic type conversion** -- bidirectional conversion between C++ and JS types (`bool`, `int`, `double`, `std::string`, `std::optional<T>`, and more)
- **RAII registration** -- builders finalize registration automatically in their destructors
- **Flexible** -- supports constructors, instance/static methods, getters, read-write properties, constants, optional arguments, and raw `JSCFunction` escape hatches

## Requirements

- C++20 compiler (MSVC, GCC, or Clang)
- CMake 3.24+
- [QuickJS](https://github.com/nicbarker/quickjs) (or QuickJS-NG)

## Integration

### As a CMake subdirectory

```cmake
add_subdirectory(path/to/qjsbind)
target_link_libraries(your_target PRIVATE qjsbind)
```

`qjsbind` is an `INTERFACE` library that links against a `qjs` CMake target. Make sure QuickJS is available as a CMake target before adding qjsbind, or place it at `third_party/quickjs/` within the qjsbind directory.

### Manual

Copy `include/qjsbind/qjsbind.h` into your project and `#include` it. You are responsible for linking QuickJS yourself.

## Quick start

```cpp
#include <qjsbind/qjsbind.h>

struct Vec2 { double x = 0, y = 0; };

void register_vec2(JSContext* ctx) {
    qjsbind::Class<Vec2>(ctx, "Vec2")
        .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> Vec2* {
            auto* v = new Vec2{};
            if (argc > 0) JS_ToFloat64(ctx, &v->x, argv[0]);
            if (argc > 1) JS_ToFloat64(ctx, &v->y, argv[1]);
            return v;
        })
        .get("x", [](Vec2* v) { return v->x; })
        .get("y", [](Vec2* v) { return v->y; })
        .method("add", [](Vec2* v, double dx, double dy) {
            v->x += dx; v->y += dy;
        }, qjsbind::returns_this)
        .method("dot", [](Vec2* v, double ox, double oy) {
            return v->x * ox + v->y * oy;
        })
        .static_method("distance", [](double x1, double y1, double x2, double y2) {
            return std::sqrt((x2-x1)*(x2-x1) + (y2-y1)*(y2-y1));
        });
}
```

```javascript
var v = new Vec2(3, 4);
v.add(1, 2);          // method chaining (returns this)
v.dot(1, 0);          // 4.0
Vec2.distance(0, 0, 3, 4); // 5.0
```

## API overview

### `Class<T>` -- expose a C++ class

| Method | Description |
|---|---|
| `.constructor(fn)` | Register a constructor: `(JSContext*, int, JSValueConst*) -> T*` |
| `.method(name, fn)` | Instance method |
| `.method(name, fn, returns_this)` | Instance method that returns `this` for chaining |
| `.get(name, fn)` | Read-only property |
| `.prop(name, getter, setter)` | Read-write property |
| `.static_method(name, fn)` | Static method on the constructor |
| `.value(name, val)` | Static constant on the constructor |
| `.method_raw(name, fn, len)` | Raw `JSCFunction` instance method |
| `.static_raw(name, fn, len)` | Raw `JSCFunction` static method |
| `.function_list(entries, count)` | Bulk `JSCFunctionListEntry` registration |

**Flags:** pass `qjsbind::NoGlobal` to skip registering on `globalThis`, or `qjsbind::NoDestructor` to skip automatic `delete` in the class finalizer.

### `Global` -- register on globalThis

```cpp
qjsbind::Global(ctx)
    .function("greet", [](std::string name) { return "Hello " + name; })
    .value("PI", 3.14159);
```

### `Namespace` -- register under a named object

```cpp
qjsbind::Namespace(ctx, "Math2")
    .function("lerp", [](double a, double b, double t) { return a + (b - a) * t; });
```

### Automatic type conversion

| C++ type | JS type |
|---|---|
| `bool` | Boolean |
| `int`, `int32_t`, `uint32_t`, `int64_t`, `uint64_t`, `float`, `double` | Number |
| `std::string`, `const char*` | String |
| `std::optional<T>` | `T` or `undefined` |
| `T*` (registered class) | Object (auto-wrapped) |
| `JSValue` | Passthrough |

### Helper functions

- `qjsbind::wrap<T>(ctx, ptr)` -- wrap an owned C++ pointer as a JS object
- `qjsbind::wrap_unowned<T>(ctx, ptr)` -- wrap a borrowed pointer (no delete on GC)
- `qjsbind::unwrap<T>(ctx, val)` -- extract a C++ pointer from a JS object

## Building the tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

QuickJS must be available as a CMake target (place sources in `third_party/quickjs/` or set `-DQJS_DIR=path/to/quickjs`).

## License

MIT License. Copyright (c) Jonny Brannum.
