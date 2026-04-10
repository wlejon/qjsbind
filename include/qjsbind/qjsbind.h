// qjsbind — C++20 header-only QuickJS binding library
// Eliminates boilerplate for exposing C++ classes to JavaScript.
#pragma once

#include "quickjs.h"

#include <cstdint>
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

// ── Detail: trampolines & call helpers ──────────────────────────────────────

namespace detail {

// Lambda storage — each unique Fn type gets its own static slot.
// Safe because each C++ lambda expression has a unique type.
template<typename Fn>
struct FnStore {
    static inline std::optional<Fn> fn;
};

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

template<typename ClassType, typename Fn, bool ReturnsThis>
JSValue method_trampoline(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv) {
    auto* self = static_cast<ClassType*>(JS_GetOpaque(this_val, class_id<ClassType>()));
    if (!self) return JS_ThrowTypeError(ctx, "invalid this");
    auto& fn = *FnStore<std::decay_t<Fn>>::fn;
    return MethodCaller<ClassType, std::decay_t<Fn>, ReturnsThis>::call(
        ctx, self, fn, argc, argv, this_val);
}

template<typename ClassType, typename Fn>
JSValue static_trampoline(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv) {
    (void)this_val;
    auto& fn = *FnStore<std::decay_t<Fn>>::fn;
    return StaticCaller<ClassType, std::decay_t<Fn>>::call(ctx, fn, argc, argv);
}

template<typename T, typename Fn>
JSValue ctor_trampoline(JSContext* ctx, JSValueConst new_target,
                        int argc, JSValueConst* argv) {
    auto& fn = *FnStore<std::decay_t<Fn>>::fn;
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

    // Constructor C function pointer (null = not constructible)
    JSCFunction* ctor_fn_ = nullptr;
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

public:
    Class(JSContext* ctx, const char* name) : ctx_(ctx), name_(name) {
        rt_ = JS_GetRuntime(ctx);
        auto& id = class_id<T>();
        if (id == 0) JS_NewClassID(rt_, &id);
        JSClassDef def{};
        def.class_name = name;
        def.finalizer = destructor;
        JS_NewClass(rt_, id, &def);
        proto_ = JS_NewObject(ctx);
    }

    Class(const Class&) = delete;
    Class& operator=(const Class&) = delete;
    Class(Class&&) = delete;
    Class& operator=(Class&&) = delete;

    ~Class() {
        // Create constructor function
        JSCFunction* fn = ctor_fn_ ? ctor_fn_ : detail::no_constructor;
        JSValue ctor = JS_NewCFunction2(ctx_, fn, name_, ctor_length_,
                                         JS_CFUNC_constructor, 0);

        // Apply deferred static methods / values
        for (auto& s : statics_)
            JS_SetPropertyStr(ctx_, ctor, s.name.c_str(), s.fn); // consumes s.fn

        // Link constructor ↔ prototype
        JS_SetConstructor(ctx_, ctor, proto_);

        // Register class prototype (consumes our proto_ ref)
        JS_SetClassProto(ctx_, class_id<T>(), proto_);

        // Put constructor on globalThis (consumes our ctor ref)
        JSValue global = JS_GetGlobalObject(ctx_);
        JS_SetPropertyStr(ctx_, global, name_, ctor);
        JS_FreeValue(ctx_, global);
    }

    // ── Constructor ─────────────────────────────────────────────────────────
    // fn signature: T*(JSContext* ctx, int argc, JSValueConst* argv)

    template<typename Fn>
    Class& constructor(Fn&& fn) {
        detail::FnStore<std::decay_t<Fn>>::fn.emplace(std::forward<Fn>(fn));
        ctor_fn_ = &detail::ctor_trampoline<T, std::decay_t<Fn>>;
        return *this;
    }

    // ── Instance methods ────────────────────────────────────────────────────
    // fn signature: (T* self, [JSContext* ctx,] args...) → R

    template<typename Fn>
    Class& method(const char* name, Fn&& fn) {
        using Caller = detail::MethodCaller<T, std::decay_t<Fn>, false>;
        detail::FnStore<std::decay_t<Fn>>::fn.emplace(std::forward<Fn>(fn));
        JS_SetPropertyStr(ctx_, proto_, name,
            JS_NewCFunction(ctx_,
                &detail::method_trampoline<T, std::decay_t<Fn>, false>,
                name, static_cast<int>(Caller::js_argc)));
        return *this;
    }

    // returns_this variant
    template<typename Fn>
    Class& method(const char* name, Fn&& fn, returns_this_t) {
        using Caller = detail::MethodCaller<T, std::decay_t<Fn>, true>;
        detail::FnStore<std::decay_t<Fn>>::fn.emplace(std::forward<Fn>(fn));
        JS_SetPropertyStr(ctx_, proto_, name,
            JS_NewCFunction(ctx_,
                &detail::method_trampoline<T, std::decay_t<Fn>, true>,
                name, static_cast<int>(Caller::js_argc)));
        return *this;
    }

    // ── Read-only property (getter only) ────────────────────────────────────
    // fn signature: (T* self, [JSContext* ctx]) → R

    template<typename Fn>
    Class& get(const char* name, Fn&& fn) {
        detail::FnStore<std::decay_t<Fn>>::fn.emplace(std::forward<Fn>(fn));
        JSAtom atom = JS_NewAtom(ctx_, name);
        JS_DefinePropertyGetSet(ctx_, proto_, atom,
            JS_NewCFunction(ctx_,
                &detail::method_trampoline<T, std::decay_t<Fn>, false>,
                name, 0),
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
        detail::FnStore<std::decay_t<GetterFn>>::fn.emplace(std::forward<GetterFn>(getter));
        detail::FnStore<std::decay_t<SetterFn>>::fn.emplace(std::forward<SetterFn>(setter));
        JSAtom atom = JS_NewAtom(ctx_, name);
        JS_DefinePropertyGetSet(ctx_, proto_, atom,
            JS_NewCFunction(ctx_,
                &detail::method_trampoline<T, std::decay_t<GetterFn>, false>,
                name, 0),
            JS_NewCFunction(ctx_,
                &detail::method_trampoline<T, std::decay_t<SetterFn>, false>,
                name, 1),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx_, atom);
        return *this;
    }

    // ── Static methods (on constructor object) ──────────────────────────────
    // fn signature: ([JSContext* ctx,] args...) → R

    template<typename Fn>
    Class& static_method(const char* name, Fn&& fn) {
        using Caller = detail::StaticCaller<T, std::decay_t<Fn>>;
        detail::FnStore<std::decay_t<Fn>>::fn.emplace(std::forward<Fn>(fn));
        statics_.push_back({
            name,
            JS_NewCFunction(ctx_,
                &detail::static_trampoline<T, std::decay_t<Fn>>,
                name, static_cast<int>(Caller::js_argc))
        });
        return *this;
    }

    // ── Static value (constant on constructor) ──────────────────────────────

    template<typename V>
    Class& value(const char* name, V val) {
        statics_.push_back({name, Convert<V>::to_js(ctx_, val)});
        return *this;
    }
};

} // namespace qjsbind
