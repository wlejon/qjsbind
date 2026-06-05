# qjsbind

A C++20 header-only binding library for [QuickJS](https://bellard.org/quickjs/), providing a fluent API to expose C++ classes and functions to JavaScript with minimal boilerplate.

## Features

- **Header-only** -- one header (`include/qjsbind/qjsbind.h`), no compilation step
- **Fluent builder API** -- register classes, methods, and properties with readable method chaining
- **Automatic type conversion** -- bidirectional conversion between C++ and JS types (`bool`, all arithmetic types, `std::string`, `const char*`, `std::optional<T>`, raw `JSValue`, and auto-wrapped registered-class pointers)
- **RAII registration** -- `Class` / `Global` / `Namespace` builders finalize registration automatically in their destructors
- **Flexible** -- supports constructors, instance/static methods, getters, read-write properties, constants, optional arguments, custom finalizers, exotic methods, GC marking, and raw `JSCFunction` escape hatches
- **Typed-array & promise helpers** -- build/read `Float32Array`/`Int32Array` buffers and create immediately-resolved/rejected promises with one call

## Requirements

- C++20 compiler (MSVC, GCC, or Clang)
- CMake 3.24+
- [QuickJS-NG](https://github.com/quickjs-ng/quickjs) (the binding is written against the QuickJS-NG API surface; classic QuickJS works where the API matches)

## Integration

### As a CMake subdirectory

```cmake
add_subdirectory(path/to/qjsbind)
target_link_libraries(your_target PRIVATE qjsbind)
```

`qjsbind` is an `INTERFACE` library that links against a `qjs` CMake target. If a `qjs` target already exists in your build, qjsbind links it directly. Otherwise the `CMakeLists.txt` looks for QuickJS sources via the `QJS_DIR` cache variable (default: the sibling `../brokit/third_party/quickjs`), falling back to `third_party/quickjs/` within the qjsbind directory.

### Manual

Copy `include/qjsbind/qjsbind.h` into your project and `#include` it. You are responsible for making `quickjs.h` available on the include path and linking QuickJS yourself.

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
| `.gc_mark(fn)` | Declare a `(T*, JSRuntime*, JS_MarkFunc*)` callback so the cycle GC can see `JSValue` fields stored in the wrapper |
| `.function_list(entries, count)` | Bulk `JSCFunctionListEntry` registration on the prototype |

Lambda signatures: methods take `(T* self, [JSContext* ctx,] args...) -> R`; static methods and `Global`/`Namespace` functions take `([JSContext* ctx,] args...) -> R`. The optional leading `JSContext*` is detected automatically. A method returning `T*` (the class's own type) is auto-wrapped into a new JS object.

**Flags:** pass `qjsbind::NoGlobal` to skip registering the constructor on `globalThis`, or `qjsbind::NoDestructor` to skip the automatic `delete` in the class finalizer. Combine with `|` (e.g. `NoGlobal | NoDestructor`).

**Custom finalizer / exotic methods:** an overloaded `Class<T>` constructor takes a `JSClassFinalizer*` and an optional `JSClassExoticMethods*` for cases where the default delete-the-opaque-pointer finalizer or standard property lookup isn't enough:

```cpp
qjsbind::Class<MyType>(ctx, "MyType", /*flags=*/0, my_finalizer, my_exotic_methods)
    .method("foo", ...);
```

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

| C++ type | JS type | Direction |
|---|---|---|
| `bool` | Boolean | both |
| any arithmetic type (`int`, `int32_t`, `uint32_t`, `int64_t`, `uint64_t`, `size_t`, `float`, `double`, …) | Number | both |
| `std::string` | String | both |
| `const char*` | String | to JS only (use `std::string` as a parameter type) |
| `std::optional<T>` | `T`, or `undefined`/`null` ↔ `std::nullopt` | both |
| `T*` (registered class) | Object (auto-wrapped/unwrapped) | both |
| `JSValue` | Passthrough | both |

### Helper functions

Wrapping:

- `qjsbind::wrap<T>(ctx, ptr)` -- wrap an owned C++ pointer as a JS object (deleted on GC)
- `qjsbind::wrap_unowned<T>(ctx, ptr)` -- wrap a borrowed pointer (no delete on GC)
- `qjsbind::unwrap<T>(ctx, val)` -- extract a `T*` from a JS object (null if the class doesn't match)
- `qjsbind::class_id<T>()` -- the per-`T` `JSClassID` (thread-local), assigned the first time a `Class<T>` is registered

Typed-array marshaling (data is copied):

- `qjsbind::make_float32_array(ctx, ptr, count)` / `make_float32_array(ctx, std::vector<float>)` -- build a `Float32Array`
- `qjsbind::make_int32_array(ctx, ptr, count)` / `make_int32_array(ctx, std::vector<int32_t>)` -- build an `Int32Array`
- `qjsbind::read_float32_array(ctx, val)` -- read a `Float32Array` view into a `std::vector<float>` (empty if not a typed array)
- `qjsbind::read_int32_array(ctx, val)` -- read an `Int32Array` view into a `std::vector<int32_t>`, falling back to a plain `Array<number>`

Promise helpers (immediate resolution; both consume the value/error reference):

- `qjsbind::make_resolved_promise(ctx, value)` -- a promise already resolved with `value`
- `qjsbind::make_rejected_promise(ctx, error)` -- a promise already rejected with `error`

For asynchronous resolution where the resolve/reject functions must outlive the call (e.g. dispatched from another thread), use `JS_NewPromiseCapability` directly.

## Building the tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Tests build only for a standalone (top-level) configure when a `qjs` target is available. QuickJS sources are resolved via `-DQJS_DIR=path/to/quickjs` (default: the sibling `../brokit/third_party/quickjs`), falling back to `third_party/quickjs/`. The suite (`tests/test_main.cpp`) registers via the CTest target `qjsbind_test`.

## License

MIT License. Copyright (c) Jonny Brannum.
