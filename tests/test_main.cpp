#include <qjsbind/qjsbind.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ── Test harness ────────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

static bool eval_ok(JSContext* ctx, const char* code, const char* name) {
    JSValue r = JS_Eval(ctx, code, strlen(code), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, exc);
        printf("  FAIL %s: %s\n", name, s ? s : "(unknown)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, r);
        return false;
    }
    JS_FreeValue(ctx, r);
    return true;
}

// For the cases where throwing *is* the answer, so an expected exception does
// not print itself as a failure.
static bool eval_throws(JSContext* ctx, const char* code) {
    JSValue r = JS_Eval(ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    const bool threw = JS_IsException(r);
    if (threw) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    return threw;
}

static void check(bool cond, const char* name) {
    if (cond) { g_passed++; }
    else { g_failed++; printf("  FAIL: %s\n", name); }
}

static bool eval_bool(JSContext* ctx, const char* code) {
    JSValue r = JS_Eval(ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    bool result = JS_ToBool(ctx, r) != 0;
    JS_FreeValue(ctx, r);
    return result;
}

static double eval_double(JSContext* ctx, const char* code) {
    JSValue r = JS_Eval(ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    double d = 0;
    JS_ToFloat64(ctx, &d, r);
    JS_FreeValue(ctx, r);
    return d;
}

static std::string eval_string(JSContext* ctx, const char* code) {
    JSValue r = JS_Eval(ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    const char* s = JS_ToCString(ctx, r);
    std::string result = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, r);
    return result;
}

// ── Test types ──────────────────────────────────────────────────────────────

struct Vec2 {
    double x = 0, y = 0;
};

struct Named {
    std::string name;
    bool visible = true;
};

// ── Tests ───────────────────────────────────────────────────────────────────

static void test_basic_class(JSContext* ctx) {
    printf("test_basic_class\n");

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
            v->x += dx;
            v->y += dy;
        }, qjsbind::returns_this)
        .method("dot", [](Vec2* v, double ox, double oy) {
            return v->x * ox + v->y * oy;
        })
        .method("length", [](Vec2* v) {
            return std::sqrt(v->x * v->x + v->y * v->y);
        })
        .static_method("zero", []() -> Vec2* {
            return new Vec2{0, 0};
        })
        .static_method("distance", [](double x1, double y1, double x2, double y2) {
            double dx = x2 - x1, dy = y2 - y1;
            return std::sqrt(dx * dx + dy * dy);
        });

    // Construction
    check(eval_ok(ctx, "var v = new Vec2(3, 4);", "construct"), "construct");
    check(eval_double(ctx, "v.x") == 3.0, "getter x");
    check(eval_double(ctx, "v.y") == 4.0, "getter y");

    // Method
    check(eval_double(ctx, "v.dot(1, 0)") == 3.0, "method dot");
    check(eval_double(ctx, "v.length()") == 5.0, "method length");

    // Returns this (chaining)
    check(eval_ok(ctx, "v.add(1, 1);", "add"), "add exec");
    check(eval_double(ctx, "v.x") == 4.0, "after add x");
    check(eval_double(ctx, "v.y") == 5.0, "after add y");
    check(eval_bool(ctx, "v.add(0,0) === v"), "returns this");

    // Static methods
    check(eval_double(ctx, "Vec2.distance(0, 0, 3, 4)") == 5.0, "static distance");
    check(eval_ok(ctx, "var z = Vec2.zero();", "static zero"), "static zero");
    check(eval_double(ctx, "z.x") == 0.0, "zero x");
    check(eval_double(ctx, "z.y") == 0.0, "zero y");
}

static void test_string_bool(JSContext* ctx) {
    printf("test_string_bool\n");

    qjsbind::Class<Named>(ctx, "Named")
        .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> Named* {
            auto* n = new Named{};
            if (argc > 0) {
                const char* s = JS_ToCString(ctx, argv[0]);
                if (s) { n->name = s; JS_FreeCString(ctx, s); }
            }
            return n;
        })
        .get("name", [](Named* n) { return n->name; })
        .prop("visible",
            [](Named* n) { return n->visible; },
            [](Named* n, bool v) { n->visible = v; })
        .method("rename", [](Named* n, std::string name) {
            n->name = std::move(name);
        }, qjsbind::returns_this);

    check(eval_ok(ctx, "var n = new Named('hello');", "named ctor"), "named ctor");
    check(eval_string(ctx, "n.name") == "hello", "string getter");

    check(eval_bool(ctx, "n.visible"), "bool getter true");
    check(eval_ok(ctx, "n.visible = false;", "bool setter"), "bool setter");
    check(!eval_bool(ctx, "n.visible"), "bool getter false");

    check(eval_ok(ctx, "n.rename('world');", "rename"), "rename");
    check(eval_string(ctx, "n.name") == "world", "string after rename");
}

static void test_optional_args(JSContext* ctx) {
    printf("test_optional_args\n");

    struct Scaler {
        double val = 1.0;
    };

    qjsbind::Class<Scaler>(ctx, "Scaler")
        .constructor([](JSContext*, int, JSValueConst*) -> Scaler* {
            return new Scaler{};
        })
        .get("val", [](Scaler* s) { return s->val; })
        .method("scale", [](Scaler* s, double factor, std::optional<double> offset) {
            s->val = s->val * factor + offset.value_or(0.0);
        }, qjsbind::returns_this);

    check(eval_ok(ctx, "var sc = new Scaler();", "scaler ctor"), "scaler ctor");
    check(eval_double(ctx, "sc.val") == 1.0, "initial val");

    check(eval_ok(ctx, "sc.scale(5);", "scale no offset"), "scale no offset");
    check(eval_double(ctx, "sc.val") == 5.0, "after scale(5)");

    check(eval_ok(ctx, "sc.scale(2, 3);", "scale with offset"), "scale with offset");
    check(eval_double(ctx, "sc.val") == 13.0, "after scale(2, 3)");
}

static void test_ctx_arg(JSContext* ctx) {
    printf("test_ctx_arg\n");

    struct Wrapper {
        int data = 42;
    };

    qjsbind::Class<Wrapper>(ctx, "Wrapper")
        .constructor([](JSContext*, int, JSValueConst*) -> Wrapper* {
            return new Wrapper{};
        })
        .method("toJSON", [](Wrapper* w, JSContext* ctx) -> JSValue {
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "data",
                              JS_NewInt32(ctx, w->data));
            return obj;
        })
        .static_method("make", [](JSContext* ctx, int val) -> JSValue {
            auto* w = new Wrapper{val};
            return qjsbind::wrap<Wrapper>(ctx, w);
        });

    check(eval_ok(ctx, "var w = new Wrapper();", "wrapper ctor"), "wrapper ctor");
    check(eval_double(ctx, "w.toJSON().data") == 42.0, "toJSON with ctx");

    check(eval_ok(ctx, "var w2 = Wrapper.make(99);", "static make"), "static make");
    check(eval_double(ctx, "w2.toJSON().data") == 99.0, "make result");
}

static void test_auto_wrap_return(JSContext* ctx) {
    printf("test_auto_wrap_return\n");

    struct Item {
        int id;
    };

    qjsbind::Class<Item>(ctx, "Item")
        .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> Item* {
            int32_t id = 0;
            if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
            return new Item{id};
        })
        .get("id", [](Item* i) { return i->id; })
        .method("clone", [](Item* i) -> Item* {
            return new Item{i->id};
        })
        .static_method("create", [](int id) -> Item* {
            return new Item{id};
        });

    check(eval_ok(ctx, "var item = new Item(7);", "item ctor"), "item ctor");
    check(eval_double(ctx, "item.id") == 7.0, "item id");

    // Method returning T* — auto-wrapped
    check(eval_ok(ctx, "var copy = item.clone();", "clone"), "clone");
    check(eval_double(ctx, "copy.id") == 7.0, "clone id");
    check(eval_bool(ctx, "copy !== item"), "clone is different object");
    check(eval_bool(ctx, "copy instanceof Item"), "clone instanceof");

    // Static returning T* — auto-wrapped
    check(eval_ok(ctx, "var made = Item.create(42);", "create"), "create");
    check(eval_double(ctx, "made.id") == 42.0, "create id");
    check(eval_bool(ctx, "made instanceof Item"), "create instanceof");
}

static void test_no_constructor(JSContext* ctx) {
    printf("test_no_constructor\n");

    struct Internal {
        int x = 0;
    };

    qjsbind::Class<Internal>(ctx, "Internal")
        .get("x", [](Internal* i) { return i->x; })
        .static_method("make", [](int val) -> Internal* {
            return new Internal{val};
        });

    // new Internal() should throw
    check(eval_throws(ctx, "new Internal();"), "no ctor throws");

    // But static factory works
    check(eval_ok(ctx, "var i = Internal.make(5);", "factory"), "factory");
    check(eval_double(ctx, "i.x") == 5.0, "factory result");
}

static void test_value(JSContext* ctx) {
    printf("test_value\n");

    struct Flags {
        int mode = 0;
    };

    qjsbind::Class<Flags>(ctx, "Flags")
        .constructor([](JSContext*, int, JSValueConst*) -> Flags* {
            return new Flags{};
        })
        .value("READ", 1)
        .value("WRITE", 2)
        .value("EXEC", 4);

    check(eval_double(ctx, "Flags.READ") == 1.0, "value READ");
    check(eval_double(ctx, "Flags.WRITE") == 2.0, "value WRITE");
    check(eval_double(ctx, "Flags.EXEC") == 4.0, "value EXEC");
}

// ── Per-registration callable storage ───────────────────────────────────────
//
// Every registration owns its own callable, which is what lets a family of
// related calls come out of one lambda expression with different captures and
// makes a function pointer as safe to register as a lambda. Both used to share
// one static slot per callable *type*, and silently: whichever was registered
// last answered for every name that shared it.

struct Sizes {
    int n = 0;
};

static double size_half(Sizes* s) { return s->n / 2.0; }
static double size_twice(Sizes* s) { return s->n * 2.0; }

static void test_registration_identity(JSContext* ctx) {
    printf("test_registration_identity\n");

    // One lambda expression, three registrations, three different captures.
    {
        qjsbind::Namespace ns(ctx, "Counters");
        static const char* const names[] = {"one", "two", "three"};
        for (int i = 0; i < 3; ++i)
            ns.function(names[i], [i](int mul) { return (i + 1) * mul; });
    }
    check(eval_double(ctx, "Counters.one(10)") == 10.0, "first of a family");
    check(eval_double(ctx, "Counters.two(10)") == 20.0, "second of a family");
    check(eval_double(ctx, "Counters.three(10)") == 30.0, "third of a family");

    // One lambda handed to two names, and a capture with a destructor.
    {
        qjsbind::Namespace ns(ctx, "Greet");
        auto greeter = [](std::string who) { return "hello " + who; };
        ns.function("en", greeter);
        ns.function("also", greeter);
        ns.function("fr", [prefix = std::string("bonjour ")](std::string who) {
            return prefix + who;
        });
    }
    check(eval_string(ctx, "Greet.en('world')") == "hello world", "one lambda, first name");
    check(eval_string(ctx, "Greet.also('world')") == "hello world", "one lambda, second name");
    check(eval_string(ctx, "Greet.fr('monde')") == "bonjour monde", "captured std::string");

    // Two function pointers of one signature — one type, two registrations.
    qjsbind::Class<Sizes>(ctx, "Sizes")
        .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> Sizes* {
            int32_t n = 0;
            if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
            return new Sizes{n};
        })
        .get("half", &size_half)
        .get("twice", &size_twice);

    check(eval_ok(ctx, "var sz = new Sizes(8);", "sizes ctor"), "sizes ctor");
    check(eval_double(ctx, "sz.half") == 4.0, "first function pointer");
    check(eval_double(ctx, "sz.twice") == 16.0, "second function pointer");

    // Still an ordinary function to JavaScript, and still a constructor that
    // says so when it is reached without `new`.
    check(eval_string(ctx, "Counters.one.name") == "one", "function keeps its name");
    check(eval_double(ctx, "Counters.one.length") == 1.0, "function keeps its length");
    check(eval_bool(ctx, "typeof Sizes === 'function'"), "constructor is a function");
    check(eval_throws(ctx, "Sizes(8);"), "constructor without new throws");
}

static void test_nested_namespace(JSContext* ctx) {
    printf("test_nested_namespace\n");

    // A namespace inside a namespace: the shape of a host surface that lives a
    // level or two down rather than on globalThis.
    {
        qjsbind::Namespace outer(ctx, "Host");
        outer.value("version", 2);
        {
            qjsbind::Namespace inner(outer, "codecs");
            inner.function("count", [] { return 7; });
        }
    }
    check(eval_double(ctx, "Host.version") == 2.0, "value on the outer namespace");
    check(eval_double(ctx, "Host.codecs.count()") == 7.0, "call on the inner one");

    // The same thing given a borrowed object rather than a namespace.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue host = JS_GetPropertyStr(ctx, global, "Host");
    {
        qjsbind::Namespace inner(ctx, host, "muxers");
        inner.function("count", [] { return 3; });
    }
    qjsbind::set_function(ctx, host, "name", [] { return std::string("qjs"); });
    JS_FreeValue(ctx, host);
    JS_FreeValue(ctx, global);
    check(eval_double(ctx, "Host.muxers.count()") == 3.0, "namespace inside a borrowed object");
    check(eval_string(ctx, "Host.name()") == "qjs", "function on a borrowed object");
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    test_basic_class(ctx);
    test_string_bool(ctx);
    test_optional_args(ctx);
    test_ctx_arg(ctx);
    test_auto_wrap_return(ctx);
    test_no_constructor(ctx);
    test_value(ctx);
    test_registration_identity(ctx);
    test_nested_namespace(ctx);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
