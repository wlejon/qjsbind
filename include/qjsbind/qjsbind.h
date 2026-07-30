// qjsbind — C++20 header-only QuickJS binding library
// Eliminates boilerplate for exposing C++ classes to JavaScript.
#pragma once

#include "quickjs.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace qjsbind {

// ── Function traits ─────────────────────────────────────────────────────────

template<typename T>
struct function_traits : function_traits<decltype(&T::operator())> {};

template<typename C, typename R, typename... Args>
struct function_traits<R(C::*)(Args...) const> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
    static constexpr size_t arity = sizeof...(Args);
};

template<typename C, typename R, typename... Args>
struct function_traits<R(C::*)(Args...)> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
    static constexpr size_t arity = sizeof...(Args);
};

template<typename R, typename... Args>
struct function_traits<R(*)(Args...)> {
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
    static constexpr size_t arity = sizeof...(Args);
};

// ── Type conversion: JSValue ↔ C++ ─────────────────────────────────────────

template<typename T, typename Enable = void>
struct Convert;

// bool
template<>
struct Convert<bool> {
    static bool from_js(JSContext* ctx, JSValue val) {
        return JS_ToBool(ctx, val) != 0;
    }
    static JSValue to_js(JSContext* ctx, bool val) {
        return JS_NewBool(ctx, val ? 1 : 0);
    }
};

// All non-bool arithmetic (int, long, float, double, size_t, etc.)
template<typename T>
struct Convert<T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>> {
    static T from_js(JSContext* ctx, JSValue val) {
        if constexpr (std::is_floating_point_v<T>) {
            double d = 0;
            JS_ToFloat64(ctx, &d, val);
            return static_cast<T>(d);
        } else if constexpr (sizeof(T) <= 4 && std::is_signed_v<T>) {
            int32_t i = 0;
            JS_ToInt32(ctx, &i, val);
            return static_cast<T>(i);
        } else if constexpr (sizeof(T) <= 4 && std::is_unsigned_v<T>) {
            uint32_t u = 0;
            JS_ToUint32(ctx, &u, val);
            return static_cast<T>(u);
        } else {
            int64_t i = 0;
            JS_ToInt64(ctx, &i, val);
            return static_cast<T>(i);
        }
    }
    static JSValue to_js(JSContext* ctx, T val) {
        if constexpr (std::is_floating_point_v<T>) {
            return JS_NewFloat64(ctx, static_cast<double>(val));
        } else if constexpr (sizeof(T) <= 4 && std::is_signed_v<T>) {
            return JS_NewInt32(ctx, static_cast<int32_t>(val));
        } else if constexpr (sizeof(T) <= 4 && std::is_unsigned_v<T>) {
            return JS_NewUint32(ctx, static_cast<uint32_t>(val));
        } else {
            return JS_NewInt64(ctx, static_cast<int64_t>(val));
        }
    }
};

// std::string
template<>
struct Convert<std::string> {
    static std::string from_js(JSContext* ctx, JSValue val) {
        const char* s = JS_ToCString(ctx, val);
        if (!s) return {};
        std::string result(s);
        JS_FreeCString(ctx, s);
        return result;
    }
    static JSValue to_js(JSContext* ctx, const std::string& val) {
        return JS_NewStringLen(ctx, val.c_str(), val.size());
    }
};

// const char* (to_js only — use std::string for from_js)
template<>
struct Convert<const char*> {
    static JSValue to_js(JSContext* ctx, const char* val) {
        return val ? JS_NewString(ctx, val) : JS_NULL;
    }
};

// JSValue passthrough
template<>
struct Convert<JSValue> {
    static JSValue from_js(JSContext*, JSValue val) { return val; }
    static JSValue to_js(JSContext*, JSValue val) { return val; }
};

// std::optional<T> — maps to undefined for missing/optional JS args
template<typename T>
struct Convert<std::optional<T>> {
    static std::optional<T> from_js(JSContext* ctx, JSValue val) {
        if (JS_IsUndefined(val) || JS_IsNull(val))
            return std::nullopt;
        return Convert<T>::from_js(ctx, val);
    }
    static JSValue to_js(JSContext* ctx, const std::optional<T>& val) {
        if (!val) return JS_UNDEFINED;
        return Convert<T>::to_js(ctx, *val);
    }
};

// ── Typed-array marshaling ──────────────────────────────────────────────────

// Build a Float32Array from a contiguous float buffer (data is copied).
inline JSValue make_float32_array(JSContext* ctx, const float* data, size_t count) {
    JSValue abuf = JS_NewArrayBufferCopy(
        ctx, reinterpret_cast<const uint8_t*>(data), count * sizeof(float));
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

// Build an Int32Array from a contiguous int32 buffer (data is copied).
inline JSValue make_int32_array(JSContext* ctx, const int32_t* data, size_t count) {
    JSValue abuf = JS_NewArrayBufferCopy(
        ctx, reinterpret_cast<const uint8_t*>(data), count * sizeof(int32_t));
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_INT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

inline JSValue make_float32_array(JSContext* ctx, const std::vector<float>& v) {
    return make_float32_array(ctx, v.data(), v.size());
}

inline JSValue make_int32_array(JSContext* ctx, const std::vector<int32_t>& v) {
    return make_int32_array(ctx, v.data(), v.size());
}

// Read a Float32Array view, falling back to a plain JS Array<number>
// (matching read_int32_array). Returns empty for anything else.
inline std::vector<float> read_float32_array(JSContext* ctx, JSValueConst val) {
    std::vector<float> out;
    if (JS_IsUndefined(val) || JS_IsNull(val)) return out;
    size_t byte_off = 0, view_len = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, val, &byte_off, &view_len, nullptr);
    if (!JS_IsException(abuf)) {
        size_t abuf_len = 0;
        uint8_t* raw = JS_GetArrayBuffer(ctx, &abuf_len, abuf);
        JS_FreeValue(ctx, abuf);
        if (raw) {
            const size_t n = view_len / sizeof(float);
            out.resize(n);
            if (n > 0) std::memcpy(out.data(), raw + byte_off, n * sizeof(float));
            return out;
        }
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }

    if (!JS_IsArray(val)) return out;
    JSValue len_v = JS_GetPropertyStr(ctx, val, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue elem = JS_GetPropertyUint32(ctx, val, i);
        double x = 0;
        JS_ToFloat64(ctx, &x, elem);
        JS_FreeValue(ctx, elem);
        out.push_back(static_cast<float>(x));
    }
    return out;
}

// Read an Int32Array view, falling back to a plain JS Array<number>.
inline std::vector<int32_t> read_int32_array(JSContext* ctx, JSValueConst val) {
    std::vector<int32_t> out;
    if (JS_IsUndefined(val) || JS_IsNull(val)) return out;

    size_t byte_off = 0, view_len = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, val, &byte_off, &view_len, nullptr);
    if (!JS_IsException(abuf)) {
        size_t abuf_len = 0;
        uint8_t* raw = JS_GetArrayBuffer(ctx, &abuf_len, abuf);
        JS_FreeValue(ctx, abuf);
        if (raw) {
            const size_t n = view_len / sizeof(int32_t);
            out.resize(n);
            if (n > 0) std::memcpy(out.data(), raw + byte_off, n * sizeof(int32_t));
            return out;
        }
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }

    if (!JS_IsArray(val)) return out;
    JSValue len_v = JS_GetPropertyStr(ctx, val, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue elem = JS_GetPropertyUint32(ctx, val, i);
        int32_t x = 0;
        JS_ToInt32(ctx, &x, elem);
        JS_FreeValue(ctx, elem);
        out.push_back(x);
    }
    return out;
}

// Zero-copy view of a typed-array's elements. Returns the element pointer and
// count without copying; {nullptr, 0} if `val` is not a typed-array view with
// the expected element size. The pointer aliases the JS ArrayBuffer — it is
// valid only until control returns to JS (any allocation/GC can move or free
// the buffer), so consume or copy it before calling back into the engine.
template<typename T>
inline const T* read_typed_array_view(JSContext* ctx, JSValueConst val, size_t& count) {
    count = 0;
    if (!JS_IsObject(val)) return nullptr;
    size_t byte_off = 0, view_len = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, val, &byte_off, &view_len, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }
    size_t abuf_len = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abuf_len, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw || bpe != sizeof(T)) return nullptr;
    count = view_len / sizeof(T);
    return reinterpret_cast<const T*>(raw + byte_off);
}

inline const float* read_float32_view(JSContext* ctx, JSValueConst val, size_t& count) {
    return read_typed_array_view<float>(ctx, val, count);
}

inline const int32_t* read_int32_view(JSContext* ctx, JSValueConst val, size_t& count) {
    return read_typed_array_view<int32_t>(ctx, val, count);
}

// ── Option-object property getters ──────────────────────────────────────────
//
// Read one property off an options object with a default. These are the
// idiomatic accessors for `fn(target, {speed: 2, loop: true})`-style APIs;
// each does the get/convert/free dance in one call and never throws.

inline double get_prop_number(JSContext* ctx, JSValueConst obj, const char* prop,
                              double def = 0.0) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    double r = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) JS_ToFloat64(ctx, &r, v);
    JS_FreeValue(ctx, v);
    return r;
}

inline int get_prop_int(JSContext* ctx, JSValueConst obj, const char* prop,
                        int def = 0) {
    return static_cast<int>(get_prop_number(ctx, obj, prop, def));
}

inline bool get_prop_bool(JSContext* ctx, JSValueConst obj, const char* prop,
                          bool def = false) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    bool r = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return r;
}

inline std::string get_prop_string(JSContext* ctx, JSValueConst obj, const char* prop,
                                   const char* def = "") {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    std::string r = def;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { r = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return r;
}

// ── Promise helpers ─────────────────────────────────────────────────────────
//
// Both helpers consume their second argument (the promise resolves/rejects
// with `value`/`error`, then both resolving functions are freed, then the
// argument is freed). For the asynchronous case where the resolve/reject
// functions need to outlive the call (e.g. dispatched back from another
// thread), keep using JS_NewPromiseCapability directly — these helpers are
// only for the immediate-resolution shape.

inline JSValue make_resolved_promise(JSContext* ctx, JSValue value) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    JS_FreeValue(ctx, value);
    return promise;
}

inline JSValue make_rejected_promise(JSContext* ctx, JSValue error) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue r = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &error);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    JS_FreeValue(ctx, error);
    return promise;
}

// ── Class ID / wrap / unwrap ────────────────────────────────────────────────

template<typename T>
JSClassID& class_id() {
    static thread_local JSClassID id = 0;
    return id;
}

template<typename T>
T* unwrap(JSContext*, JSValue val) {
    return static_cast<T*>(JS_GetOpaque(val, class_id<T>()));
}

template<typename T>
JSValue wrap(JSContext* ctx, T* ptr) {
    if (!ptr) return JS_NULL;
    JSValue proto = JS_GetClassProto(ctx, class_id<T>());
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, class_id<T>());
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        delete ptr;
        return obj;
    }
    JS_SetOpaque(obj, ptr);
    return obj;
}

// ── Tag type for returns-this methods ───────────────────────────────────────

struct returns_this_t {};
inline constexpr returns_this_t returns_this{};

// ── Class registration flags ───────────────────────────────────────────────

enum ClassFlag : unsigned {
    NoGlobal     = 1u << 0,  // Don't register constructor on globalThis
    NoDestructor = 1u << 1,  // Don't delete opaque pointer in finalizer
};

constexpr unsigned operator|(ClassFlag a, ClassFlag b) {
    return static_cast<unsigned>(a) | static_cast<unsigned>(b);
}
constexpr unsigned operator|(unsigned a, ClassFlag b) {
    return a | static_cast<unsigned>(b);
}

// ── Wrap without ownership (for borrowed pointers) ─────────────────────────

template<typename T>
JSValue wrap_unowned(JSContext* ctx, T* ptr) {
    if (!ptr) return JS_NULL;
    JSValue proto = JS_GetClassProto(ctx, class_id<T>());
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, class_id<T>());
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, ptr);
    return obj;
}

// ── Detail: callable storage, trampolines & call helpers ────────────────────

namespace detail {

// Where a registered callable lives: on the function object that calls it.
//
// A closure has to outlive the registration call and die with the JS function
// it was registered as, and QuickJS hands us exactly that — `JS_NewCClosure`
// carries a `void*` and a finalizer for it. So a registration heap-allocates
// its callable and the function object owns it. Two registrations never share
// storage, a second runtime installing the same bindings gets its own captures,
// and the callable is freed when the function is collected.
//
// This replaced one `static` slot per callable *type*, which was wrong three
// ways and silent in all of them. A function pointer types the same for every
// function of its signature, so `.method("a", &f).method("b", &g)` gave both
// names whichever was registered last. One lambda handed to two names was one
// slot. And a lambda whose captures differ per registration — the obvious way
// to write a family of related calls, a loop over a table of names — kept only
// the last set. Nothing diagnosed any of the three: the call succeeded and
// answered about the wrong thing.

/// Free a callable its function object no longer needs. `opaque` is the `Fn*`
/// the registration allocated, so the delete is typed and needs no vtable.
template<typename Fn>
void delete_callable(void* opaque) {
    delete static_cast<Fn*>(opaque);
}

/// A function object owning its own copy of `fn`. `name` and `length` are set
/// the way `JS_NewCFunction` sets them, so `f.name` and `f.length` read the
/// same as a plain C function's.
///
/// If the allocation inside QuickJS fails the callable leaks rather than being
/// deleted here: one of the two failure paths attaches the record before
/// throwing and has therefore already run the finalizer, and leaking once on
/// OOM is the better of the two mistakes.
template<typename Fn>
JSValue new_closure(JSContext* ctx, const char* name, int length,
                    JSCClosure* tramp, Fn&& fn) {
    using D = std::decay_t<Fn>;
    return JS_NewCClosure(ctx, tramp, name, &delete_callable<D>, length, 0,
                          new D(std::forward<Fn>(fn)));
}

inline JSValue safe_arg(int argc, JSValueConst* argv, int index) {
    return (index < argc) ? argv[index] : JS_UNDEFINED;
}

// Safe tuple_element access — returns void for out-of-bounds indices
template<size_t I, typename Tuple>
consteval bool is_jscontext_at() {
    if constexpr (I < std::tuple_size_v<Tuple>)
        return std::is_same_v<std::tuple_element_t<I, Tuple>, JSContext*>;
    else
        return false;
}

// Return-value conversion with auto-wrap for T* matching ClassType
template<typename ClassType, typename R>
JSValue convert_return(JSContext* ctx, R&& val) {
    using D = std::decay_t<R>;
    if constexpr (std::is_same_v<D, JSValue>) {
        return val;
    } else if constexpr (std::is_pointer_v<D> &&
                          std::is_same_v<std::remove_pointer_t<D>, ClassType>) {
        return val ? wrap<ClassType>(ctx, val) : JS_NULL;
    } else {
        return Convert<D>::to_js(ctx, std::forward<R>(val));
    }
}

// ── MethodCaller: (Self*, [JSContext*,] args...) → R ────────────────────────

template<typename ClassType, typename Fn, bool ReturnsThis>
struct MethodCaller {
    using traits = function_traits<std::decay_t<Fn>>;
    using args_t = typename traits::args_tuple;
    using ret    = typename traits::return_type;

    static constexpr size_t total = traits::arity;
    static constexpr bool has_ctx = is_jscontext_at<1, args_t>();
    static constexpr size_t skip = 1 + (has_ctx ? 1 : 0);
    static constexpr size_t js_argc = total - skip;

    template<size_t I>
    static auto arg(JSContext* ctx, int argc, JSValueConst* argv) {
        using A = std::decay_t<std::tuple_element_t<I + skip, args_t>>;
        return Convert<A>::from_js(ctx, safe_arg(argc, argv, static_cast<int>(I)));
    }

    template<size_t... Is>
    static JSValue invoke(JSContext* ctx,
                          std::tuple_element_t<0, args_t> self,
                          Fn& fn, int argc, JSValueConst* argv,
                          JSValue this_val, std::index_sequence<Is...>) {
        if constexpr (ReturnsThis) {
            if constexpr (has_ctx) fn(self, ctx, arg<Is>(ctx, argc, argv)...);
            else                   fn(self, arg<Is>(ctx, argc, argv)...);
            return JS_DupValue(ctx, this_val);
        } else if constexpr (std::is_void_v<ret>) {
            if constexpr (has_ctx) fn(self, ctx, arg<Is>(ctx, argc, argv)...);
            else                   fn(self, arg<Is>(ctx, argc, argv)...);
            return JS_UNDEFINED;
        } else {
            ret r;
            if constexpr (has_ctx) r = fn(self, ctx, arg<Is>(ctx, argc, argv)...);
            else                   r = fn(self, arg<Is>(ctx, argc, argv)...);
            return convert_return<ClassType>(ctx, std::move(r));
        }
    }

    static JSValue call(JSContext* ctx,
                        std::tuple_element_t<0, args_t> self,
                        Fn& fn, int argc, JSValueConst* argv,
                        JSValue this_val) {
        return invoke(ctx, self, fn, argc, argv, this_val,
                      std::make_index_sequence<js_argc>{});
    }
};

// ── StaticCaller: ([JSContext*,] args...) → R ───────────────────────────────

template<typename ClassType, typename Fn>
struct StaticCaller {
    using traits = function_traits<std::decay_t<Fn>>;
    using args_t = typename traits::args_tuple;
    using ret    = typename traits::return_type;

    static constexpr size_t total = traits::arity;
    static constexpr bool has_ctx = is_jscontext_at<0, args_t>();
    static constexpr size_t skip = has_ctx ? 1 : 0;
    static constexpr size_t js_argc = total - skip;

    template<size_t I>
    static auto arg(JSContext* ctx, int argc, JSValueConst* argv) {
        using A = std::decay_t<std::tuple_element_t<I + skip, args_t>>;
        return Convert<A>::from_js(ctx, safe_arg(argc, argv, static_cast<int>(I)));
    }

    template<size_t... Is>
    static JSValue invoke(JSContext* ctx, Fn& fn, int argc, JSValueConst* argv,
                          std::index_sequence<Is...>) {
        if constexpr (std::is_void_v<ret>) {
            if constexpr (has_ctx) fn(ctx, arg<Is>(ctx, argc, argv)...);
            else                   fn(arg<Is>(ctx, argc, argv)...);
            return JS_UNDEFINED;
        } else {
            ret r;
            if constexpr (has_ctx) r = fn(ctx, arg<Is>(ctx, argc, argv)...);
            else                   r = fn(arg<Is>(ctx, argc, argv)...);
            return convert_return<ClassType>(ctx, std::move(r));
        }
    }

    static JSValue call(JSContext* ctx, Fn& fn, int argc, JSValueConst* argv) {
        return invoke(ctx, fn, argc, argv, std::make_index_sequence<js_argc>{});
    }
};

// Workaround: StaticCaller for zero-arg lambdas (tuple_element_t<0, empty> is ill-formed)
template<typename ClassType, typename Fn>
    requires (function_traits<std::decay_t<Fn>>::arity == 0)
struct StaticCaller<ClassType, Fn> {
    using traits = function_traits<std::decay_t<Fn>>;
    using ret = typename traits::return_type;
    static constexpr size_t js_argc = 0;

    static JSValue call(JSContext* ctx, Fn& fn, int, JSValueConst*) {
        if constexpr (std::is_void_v<ret>) {
            fn();
            return JS_UNDEFINED;
        } else {
            return convert_return<ClassType>(ctx, fn());
        }
    }
};

// ── Trampolines (C function pointers registered with QuickJS) ──────────────
//
// One instantiation per callable type, and the callable itself arrives as the
// `opaque` its function object owns — see `new_closure`.

template<typename ClassType, typename Fn, bool ReturnsThis>
JSValue method_trampoline(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv, int, void* opaque) {
    auto* self = static_cast<ClassType*>(JS_GetOpaque(this_val, class_id<ClassType>()));
    if (!self) return JS_ThrowTypeError(ctx, "invalid this");
    auto& fn = *static_cast<std::decay_t<Fn>*>(opaque);
    return MethodCaller<ClassType, std::decay_t<Fn>, ReturnsThis>::call(
        ctx, self, fn, argc, argv, this_val);
}

template<typename ClassType, typename Fn>
JSValue static_trampoline(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv, int, void* opaque) {
    (void)this_val;
    auto& fn = *static_cast<std::decay_t<Fn>*>(opaque);
    return StaticCaller<ClassType, std::decay_t<Fn>>::call(ctx, fn, argc, argv);
}

// A constructor is called with `new_target` where a method gets `this`, and
// unlike `JS_CFUNC_constructor` a closure is not told which of the two it is —
// so the check that it was reached through `new` is made here, and stays a
// sentence rather than becoming a read of `undefined.prototype`.
template<typename T, typename Fn>
JSValue ctor_trampoline(JSContext* ctx, JSValueConst new_target,
                        int argc, JSValueConst* argv, int, void* opaque) {
    if (!JS_IsConstructor(ctx, new_target))
        return JS_ThrowTypeError(ctx, "must be called with new");
    auto& fn = *static_cast<std::decay_t<Fn>*>(opaque);
    T* obj = fn(ctx, argc, argv);
    if (!obj) return JS_EXCEPTION;
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue jsobj = JS_NewObjectProtoClass(ctx, proto, class_id<T>());
    JS_FreeValue(ctx, proto);
    if (JS_IsException(jsobj)) {
        delete obj;
        return jsobj;
    }
    JS_SetOpaque(jsobj, obj);
    return jsobj;
}

inline JSValue no_constructor(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_ThrowTypeError(ctx, "not constructible");
}

} // namespace detail

// ── Free functions on an object that already exists ─────────────────────────

/// Put an auto-converting function on `obj`: `([JSContext*,] args...) → R`.
///
/// What `Global` and `Namespace` are made of, public because a binding that was
/// handed an object from somewhere else — a prototype, a namespace another part
/// of the host built — needs the same thing and should not have to reach into
/// `detail` for it.
template<typename Fn>
    requires (!std::is_convertible_v<std::decay_t<Fn>, JSCFunction*>)
void set_function(JSContext* ctx, JSValue obj, const char* name, Fn&& fn) {
    using Caller = detail::StaticCaller<void, std::decay_t<Fn>>;
    JS_SetPropertyStr(ctx, obj, name,
        detail::new_closure(ctx, name, static_cast<int>(Caller::js_argc),
                            &detail::static_trampoline<void, std::decay_t<Fn>>,
                            std::forward<Fn>(fn)));
}

// ── Class<T> builder ────────────────────────────────────────────────────────
//
// Usage (temporary — destructor finalizes registration):
//
//   qjsbind::Class<MyType>(ctx, "MyType")
//       .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> MyType* { ... })
//       .get("name", [](MyType* self) { return self->name; })
//       .prop("x", [](MyType* s) { return s->x; }, [](MyType* s, double v) { s->x = v; })
//       .method("foo", [](MyType* self, double x) { ... })
//       .method("bar", [](MyType* self, int x) { ... }, qjsbind::returns_this)
//       .static_method("create", [](JSContext* ctx) -> MyType* { ... });
//

template<typename T>
class Class {
    JSContext* ctx_;
    JSRuntime* rt_;
    JSValue proto_;
    const char* name_;
    unsigned flags_ = 0;

    // The constructor function, built by `constructor()` because it owns the
    // callable (undefined = not constructible).
    JSValue ctor_ = JS_UNDEFINED;
    int ctor_length_ = 0;

    // Deferred statics (applied to constructor in destructor)
    struct StaticEntry {
        std::string name;
        JSValue fn;
    };
    std::vector<StaticEntry> statics_;

    static void destructor(JSRuntime*, JSValue val) {
        auto* ptr = static_cast<T*>(JS_GetOpaque(val, class_id<T>()));
        delete ptr;
    }

    // User-supplied gc_mark callback for instances of T. Stored per-T so the
    // single trampoline registered on the class can dispatch into it.
    // Without a gc_mark, QuickJS's cycle GC can't see JSValue fields stored
    // inside the C++ wrapper, so wrapper ↔ stored-callback cycles leak.
    using GcMarkFn = void (*)(T*, JSRuntime*, JS_MarkFunc*);
    static GcMarkFn& gc_mark_fn() {
        static GcMarkFn fn = nullptr;
        return fn;
    }

    static void gc_mark_trampoline(JSRuntime* rt, JSValueConst val,
                                   JS_MarkFunc* mark_func) {
        if (auto fn = gc_mark_fn()) {
            auto* ptr = static_cast<T*>(JS_GetOpaque(val, class_id<T>()));
            if (ptr) fn(ptr, rt, mark_func);
        }
    }

public:
    /// Basic constructor (default delete-ptr finalizer unless NoDestructor).
    Class(JSContext* ctx, const char* name, unsigned flags = 0)
        : ctx_(ctx), name_(name), flags_(flags) {
        rt_ = JS_GetRuntime(ctx);
        auto& id = class_id<T>();
        if (id == 0) JS_NewClassID(rt_, &id);
        JSClassDef def{};
        def.class_name = name;
        def.finalizer = (flags & NoDestructor) ? nullptr : destructor;
        def.gc_mark = gc_mark_trampoline;
        JS_NewClass(rt_, id, &def);
        proto_ = JS_NewObject(ctx);
    }

    /// Extended constructor with custom finalizer and/or exotic methods.
    /// If finalizer is non-null it overrides the default (and NoDestructor flag).
    Class(JSContext* ctx, const char* name, unsigned flags,
          JSClassFinalizer* finalizer,
          JSClassExoticMethods* exotic = nullptr)
        : ctx_(ctx), name_(name), flags_(flags) {
        rt_ = JS_GetRuntime(ctx);
        auto& id = class_id<T>();
        if (id == 0) JS_NewClassID(rt_, &id);
        JSClassDef def{};
        def.class_name = name;
        def.finalizer = finalizer ? finalizer
                      : ((flags & NoDestructor) ? nullptr : destructor);
        def.gc_mark = gc_mark_trampoline;
        def.exotic = exotic;
        JS_NewClass(rt_, id, &def);
        proto_ = JS_NewObject(ctx);
    }

    Class(const Class&) = delete;
    Class& operator=(const Class&) = delete;
    Class(Class&&) = delete;
    Class& operator=(Class&&) = delete;

    ~Class() {
        // The constructor function, or one that says the class has none
        JSValue ctor = JS_IsUndefined(ctor_)
            ? JS_NewCFunction2(ctx_, detail::no_constructor, name_, ctor_length_,
                               JS_CFUNC_constructor, 0)
            : ctor_;

        // Apply deferred static methods / values
        for (auto& s : statics_)
            JS_SetPropertyStr(ctx_, ctor, s.name.c_str(), s.fn); // consumes s.fn

        // Link constructor ↔ prototype
        JS_SetConstructor(ctx_, ctor, proto_);

        // Register class prototype (consumes our proto_ ref)
        JS_SetClassProto(ctx_, class_id<T>(), proto_);

        if (flags_ & NoGlobal) {
            // Don't register on globalThis — free the constructor
            JS_FreeValue(ctx_, ctor);
        } else {
            // Put constructor on globalThis (consumes our ctor ref)
            JSValue global = JS_GetGlobalObject(ctx_);
            JS_SetPropertyStr(ctx_, global, name_, ctor);
            JS_FreeValue(ctx_, global);
        }
    }

    // ── Constructor ─────────────────────────────────────────────────────────
    // fn signature: T*(JSContext* ctx, int argc, JSValueConst* argv)

    template<typename Fn>
    Class& constructor(Fn&& fn) {
        if (!JS_IsUndefined(ctor_)) JS_FreeValue(ctx_, ctor_);
        ctor_ = detail::new_closure(ctx_, name_, ctor_length_,
                                    &detail::ctor_trampoline<T, std::decay_t<Fn>>,
                                    std::forward<Fn>(fn));
        // A closure is a callable object and nothing more until this is set;
        // `new Thing()` checks the bit before it dispatches.
        JS_SetConstructorBit(ctx_, ctor_, true);
        return *this;
    }

    // ── Instance methods ────────────────────────────────────────────────────
    // fn signature: (T* self, [JSContext* ctx,] args...) → R

    template<typename Fn>
    Class& method(const char* name, Fn&& fn) {
        using Caller = detail::MethodCaller<T, std::decay_t<Fn>, false>;
        JS_SetPropertyStr(ctx_, proto_, name,
            detail::new_closure(ctx_, name, static_cast<int>(Caller::js_argc),
                &detail::method_trampoline<T, std::decay_t<Fn>, false>,
                std::forward<Fn>(fn)));
        return *this;
    }

    // returns_this variant
    template<typename Fn>
    Class& method(const char* name, Fn&& fn, returns_this_t) {
        using Caller = detail::MethodCaller<T, std::decay_t<Fn>, true>;
        JS_SetPropertyStr(ctx_, proto_, name,
            detail::new_closure(ctx_, name, static_cast<int>(Caller::js_argc),
                &detail::method_trampoline<T, std::decay_t<Fn>, true>,
                std::forward<Fn>(fn)));
        return *this;
    }

    // ── Read-only property (getter only) ────────────────────────────────────
    // fn signature: (T* self, [JSContext* ctx]) → R

    template<typename Fn>
    Class& get(const char* name, Fn&& fn) {
        JSAtom atom = JS_NewAtom(ctx_, name);
        JS_DefinePropertyGetSet(ctx_, proto_, atom,
            detail::new_closure(ctx_, name, 0,
                &detail::method_trampoline<T, std::decay_t<Fn>, false>,
                std::forward<Fn>(fn)),
            JS_UNDEFINED,
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx_, atom);
        return *this;
    }

    // ── Read-write property (getter + setter) ───────────────────────────────
    // getter: (T* self, [JSContext* ctx]) → R
    // setter: (T* self, [JSContext* ctx,] V value) → void

    template<typename GetterFn, typename SetterFn>
    Class& prop(const char* name, GetterFn&& getter, SetterFn&& setter) {
        JSAtom atom = JS_NewAtom(ctx_, name);
        JS_DefinePropertyGetSet(ctx_, proto_, atom,
            detail::new_closure(ctx_, name, 0,
                &detail::method_trampoline<T, std::decay_t<GetterFn>, false>,
                std::forward<GetterFn>(getter)),
            detail::new_closure(ctx_, name, 1,
                &detail::method_trampoline<T, std::decay_t<SetterFn>, false>,
                std::forward<SetterFn>(setter)),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx_, atom);
        return *this;
    }

    // ── Static methods (on constructor object) ──────────────────────────────
    // fn signature: ([JSContext* ctx,] args...) → R

    template<typename Fn>
    Class& static_method(const char* name, Fn&& fn) {
        using Caller = detail::StaticCaller<T, std::decay_t<Fn>>;
        statics_.push_back({
            name,
            detail::new_closure(ctx_, name, static_cast<int>(Caller::js_argc),
                &detail::static_trampoline<T, std::decay_t<Fn>>,
                std::forward<Fn>(fn))
        });
        return *this;
    }

    // ── Raw method — escape hatch for complex arg parsing ─────────────────
    // fn is a standard JSCFunction: (ctx, this_val, argc, argv) → JSValue

    Class& method_raw(const char* name, JSCFunction* fn, int length = 0) {
        JS_SetPropertyStr(ctx_, proto_, name,
            JS_NewCFunction(ctx_, fn, name, length));
        return *this;
    }

    // ── GC mark — declare which JSValue fields the cycle GC must traverse ──
    // Without this, JSValues stored inside the C++ wrapper are invisible to
    // QuickJS's cycle GC, so wrapper ↔ stored-callback cycles leak. The
    // callback should call JS_MarkValue on each JSValue field that holds a
    // reference (stored via JS_DupValue).
    //
    // Example:
    //   Class<MyData>(ctx, "MyData")
    //       .gc_mark([](MyData* d, JSRuntime* rt, JS_MarkFunc* mark) {
    //           JS_MarkValue(rt, d->callback, mark);
    //       })
    Class& gc_mark(GcMarkFn fn) {
        gc_mark_fn() = fn;
        return *this;
    }

    // ── Raw static method — escape hatch for complex arg parsing ────────

    Class& static_raw(const char* name, JSCFunction* fn, int length = 0) {
        statics_.push_back({name, JS_NewCFunction(ctx_, fn, name, length)});
        return *this;
    }

    // ── Static value (constant on constructor) ──────────────────────────────

    template<typename V>
    Class& value(const char* name, V val) {
        statics_.push_back({name, Convert<V>::to_js(ctx_, val)});
        return *this;
    }

    // ── Raw function list — apply JSCFunctionListEntry array to prototype ──

    Class& function_list(const JSCFunctionListEntry* entries, int count) {
        JS_SetPropertyFunctionList(ctx_, proto_, entries, count);
        return *this;
    }
};

// ── Global builder ─────────────────────────────────────────────────────────
//
// RAII builder for registering functions/values on globalThis.
//
//   qjsbind::Global(ctx)
//       .function("setTimeout", js_setTimeout, 2)
//       .function("greet", [](std::string name) { return "Hello " + name; })
//       .value("PI", 3.14159);
//

class Global {
    JSContext* ctx_;
    JSValue global_;

public:
    explicit Global(JSContext* ctx) : ctx_(ctx), global_(JS_GetGlobalObject(ctx)) {}
    ~Global() { JS_FreeValue(ctx_, global_); }

    Global(const Global&) = delete;
    Global& operator=(const Global&) = delete;
    Global(Global&&) = delete;
    Global& operator=(Global&&) = delete;

    /// Raw JSCFunction
    Global& function(const char* name, JSCFunction* fn, int length = 0) {
        JS_SetPropertyStr(ctx_, global_, name,
            JS_NewCFunction(ctx_, fn, name, length));
        return *this;
    }

    /// Auto-converting typed lambda: ([JSContext*,] args...) → R
    template<typename Fn>
        requires (!std::is_convertible_v<std::decay_t<Fn>, JSCFunction*>)
    Global& function(const char* name, Fn&& fn) {
        set_function(ctx_, global_, name, std::forward<Fn>(fn));
        return *this;
    }

    /// Raw JSValue (consumes the reference)
    Global& value(const char* name, JSValue val) {
        JS_SetPropertyStr(ctx_, global_, name, val);
        return *this;
    }

    /// Typed value
    template<typename V>
        requires (!std::is_same_v<std::decay_t<V>, JSValue>)
    Global& value(const char* name, V val) {
        JS_SetPropertyStr(ctx_, global_, name, Convert<V>::to_js(ctx_, val));
        return *this;
    }

    /// Apply JSCFunctionListEntry array
    Global& function_list(const JSCFunctionListEntry* entries, int count) {
        JS_SetPropertyFunctionList(ctx_, global_, entries, count);
        return *this;
    }

    /// Access the global object for custom manipulation
    JSValue object() const { return global_; }

    /// The context this is registering into
    JSContext* context() const { return ctx_; }
};

// ── Namespace builder ──────────────────────────────────────────────────────
//
// RAII builder that creates a named object on globalThis, or inside an object
// you already have. The object is attached in the destructor.
//
//   qjsbind::Namespace(ctx, "Physics")
//       .function("setGravity", js_setGravity, 3)
//       .function("step", [](double dt) { world->step(dt); })
//       .function_list(physics_funcs, physics_funcs_count);
//
// A namespace that is not on globalThis takes its parent, because a host
// surface is often a level or two down — `bro.ffmpeg`, and `render` inside
// that. The parent is *borrowed*: it must outlive the namespace, which for a
// nested one means the inner scope closes first.
//
//   qjsbind::Namespace media(ctx, some_obj, "media");
//   { qjsbind::Namespace codecs(media, "codecs"); codecs.function(...); }
//

class Namespace {
    JSContext* ctx_;
    JSValue obj_;
    JSValue parent_;  // undefined = globalThis, read in the destructor
    const char* name_;

public:
    Namespace(JSContext* ctx, const char* name)
        : ctx_(ctx), obj_(JS_NewObject(ctx)), parent_(JS_UNDEFINED), name_(name) {}

    /// A namespace inside a borrowed object rather than on globalThis.
    Namespace(JSContext* ctx, JSValue parent, const char* name)
        : ctx_(ctx), obj_(JS_NewObject(ctx)), parent_(parent), name_(name) {}

    /// A namespace inside another namespace.
    Namespace(Namespace& parent, const char* name)
        : Namespace(parent.ctx_, parent.obj_, name) {}

    ~Namespace() {
        if (JS_IsUndefined(parent_)) {
            JSValue global = JS_GetGlobalObject(ctx_);
            JS_SetPropertyStr(ctx_, global, name_, obj_);
            JS_FreeValue(ctx_, global);
        } else {
            JS_SetPropertyStr(ctx_, parent_, name_, obj_);
        }
    }

    Namespace(const Namespace&) = delete;
    Namespace& operator=(const Namespace&) = delete;
    Namespace(Namespace&&) = delete;
    Namespace& operator=(Namespace&&) = delete;

    /// Raw JSCFunction
    Namespace& function(const char* name, JSCFunction* fn, int length = 0) {
        JS_SetPropertyStr(ctx_, obj_, name,
            JS_NewCFunction(ctx_, fn, name, length));
        return *this;
    }

    /// Auto-converting typed lambda: ([JSContext*,] args...) → R
    template<typename Fn>
        requires (!std::is_convertible_v<std::decay_t<Fn>, JSCFunction*>)
    Namespace& function(const char* name, Fn&& fn) {
        set_function(ctx_, obj_, name, std::forward<Fn>(fn));
        return *this;
    }

    /// Apply JSCFunctionListEntry array
    Namespace& function_list(const JSCFunctionListEntry* entries, int count) {
        JS_SetPropertyFunctionList(ctx_, obj_, entries, count);
        return *this;
    }

    /// Raw JSValue (consumes the reference)
    Namespace& value(const char* name, JSValue val) {
        JS_SetPropertyStr(ctx_, obj_, name, val);
        return *this;
    }

    /// Typed value
    template<typename V>
        requires (!std::is_same_v<std::decay_t<V>, JSValue>)
    Namespace& value(const char* name, V val) {
        JS_SetPropertyStr(ctx_, obj_, name, Convert<V>::to_js(ctx_, val));
        return *this;
    }

    /// Access the namespace object for custom manipulation
    JSValue object() const { return obj_; }

    /// The context this is registering into
    JSContext* context() const { return ctx_; }
};

} // namespace qjsbind
