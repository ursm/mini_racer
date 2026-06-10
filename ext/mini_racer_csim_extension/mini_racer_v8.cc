#include "v8.h"
#include "v8-profiler.h"
#include "libplatform/libplatform.h"
#include "mini_racer_v8.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// note: the filter function gets called inside the safe context,
// i.e., the context that has not been tampered with by user JS
// convention: $-prefixed identifiers signify objects from the
// user JS context and should be handled with special care
static const char safe_context_script_source[] = R"js(
;(function($globalThis) {
    const {Map: $Map, Set: $Set} = $globalThis
    const sentinel = {}
    return function filter(root) {
        // Memoize original -> filtered copy. Registered BEFORE recursing into a
        // value's children so that a value reachable by many paths is cloned
        // once (linear, not super-linear: e.g. a DOM node's ownerDocument is
        // reachable from every node), cycles terminate (the in-progress copy is
        // returned), and object identity is preserved. This matches V8's
        // ValueSerializer (the fast path this is the fallback for), whose ref
        // table also dedupes and handles cycles; without it the slow path both
        // diverges (duplicates shared objects) and can recurse forever on a
        // cyclic value that happens to contain a non-cloneable member.
        const seen = new Map()
        return (function rec(v) {
            if (typeof v === "function")
                return sentinel
            if (typeof v !== "object" || v === null)
                return v
            if (seen.has(v))
                return seen.get(v)
            if (v instanceof $Map) {
                const m = new Map()
                seen.set(v, m)
                for (let [k, t] of Map.prototype.entries.call(v)) {
                    t = rec(t)
                    if (t !== sentinel)
                        m.set(k, t)
                }
                return m
            } else if (v instanceof $Set) {
                const s = new Set()
                seen.set(v, s)
                for (let t of Set.prototype.values.call(v)) {
                    t = rec(t)
                    if (t !== sentinel)
                        s.add(t)
                }
                return s
            } else {
                const o = Array.isArray(v) ? [] : {}
                seen.set(v, o)
                const pds = Object.getOwnPropertyDescriptors(v)
                for (const [k, d] of Object.entries(pds)) {
                    if (!d.enumerable)
                        continue
                    let t = d.value
                    if (d.get) {
                        // *not* d.get.call(...), may have been tampered with
                        t = Function.prototype.call.call(d.get, v, k)
                    }
                    t = rec(t)
                    if (t !== sentinel)
                        Object.defineProperty(o, k, {value: t, enumerable: true})
                }
                return o
            }
        })(root)
    }
})
)js";

struct Callback
{
    struct State *st;
    int32_t id;
    // The dotted global path the callback was attached at (e.g. "foo.bar").
    // Retained so the JS shim function can be re-bound onto a fresh global
    // after Context#reset_realm swaps the user realm.
    std::string name;
};

// V8 doesn't expose ScriptOrigin's filename back from a v8::Module
// (UnboundModuleScript only exposes the //# sourceURL magic comment),
// so we cache the filename here at compile time. Used to populate the
// referrer URL passed to the Ruby resolver block and `import.meta.url`.
// v8::Global (not Persistent) so ~ModuleEntry releases the V8 handle
// eagerly — Persistent's default traits skip Reset() in the destructor.
struct ModuleEntry
{
    v8::Global<v8::Module> handle;
    std::string filename;
};

// Transient per-load resolution map, reachable from graph_resolve_callback
// (which has no embedder slot) via State::active_graph. The modules themselves
// live in the persistent URL registry (State::module_id_by_url); this only
// records how each import edge resolved during the walk.
struct GraphLoad
{
    // referrer_url '\0' specifier -> resolved url ("" = embedder returned nil)
    std::unordered_map<std::string, std::string> edges;
};

// NOTE: do *not* use thread_locals to store state. In single-threaded
// mode, V8 runs on the same thread as Ruby and the Ruby runtime clobbers
// thread-locals when it context-switches threads. Ruby 3.4.0 has a new
// API rb_thread_lock_native_thread() that pins the thread but I don't
// think we're quite ready yet to drop support for older versions, hence
// this inelegant "everything" struct.
// A single V8 realm (v8::Context) plus the script/module bookkeeping bound to
// it. mini_racer runs one "main" realm (id 0); Context#create_realm adds more
// (per-frame realms in the same isolate, like browser iframes). Each realm has
// its own user context, its companion safe-context marshalling function, and
// its own script/module/url registries because those handles are realm-bound.
struct Realm
{
    // Canonical roots for this realm. The State::context/safe_context/
    // safe_context_function Locals are re-derived from these every request
    // (see restore_realm_locals); swapping/selecting a realm is just a matter
    // of which Realm's persistents we re-derive from.
    v8::Persistent<v8::Context> persistent_context;
    v8::Persistent<v8::Context> persistent_safe_context;
    v8::Persistent<v8::Function> persistent_safe_context_function;
    // v8::Global (not Persistent): Global's destructor Reset()s the handle,
    // so erase()/clear() actually release the compiled script eagerly.
    std::unordered_map<int32_t, v8::Global<v8::Script>> scripts;
    int32_t next_script_id = 0;
    // ModuleEntry holds v8::Global<Module> + cached filename.
    std::unordered_map<int32_t, std::unique_ptr<ModuleEntry>> modules;
    int32_t next_module_id = 0;
    // "1 URL = 1 Module" registry (url -> id into `modules`) shared across every
    // load path for this realm's lifetime; cleared with `modules`.
    std::unordered_map<std::string, int32_t> module_id_by_url;
    // True once load_module_graph has run for this realm: routes dynamic
    // import() through the URL registry instead of the legacy resolver.
    bool uses_graph_loader = false;
    // Set for the duration of v8_load_module_graph's InstantiateModule call so
    // graph_resolve_callback can resolve imports from the pre-walked graph
    // with zero Ruby round-trips. Null otherwise.
    struct GraphLoad *active_graph = nullptr;
    // Embedder-registered handler for promises that reject with no handler in
    // this realm, set via <host_namespace>.onUnhandledRejection(fn). Called by
    // notify_unhandled_rejections with (reason, promise). Global so its dtor
    // releases the function when the realm is disposed; reset on reset_realm.
    v8::Global<v8::Function> unhandled_rejection_handler;
};

struct State
{
    v8::Isolate *isolate;
    // Locals for the *active* realm (cur(*this)), re-derived each request from
    // that Realm's persistents (see restore_realm_locals). `context` is the
    // active user realm; `safe_context` is the trusted built-ins context.
    v8::Local<v8::Context> context;
    v8::Local<v8::Context> safe_context;
    v8::Local<v8::Function> safe_context_function;
    v8::Persistent<v8::Value> ruby_exception;
    // One security token shared by every realm in this isolate so per-frame
    // realms (Context#create_realm) can reach each other's globals like
    // same-origin iframes. Captured from the first realm's default token.
    v8::Persistent<v8::Value> shared_security_token;
    // The opt-in host namespace name (Context.new(host_namespace:)), retained
    // so it can be re-installed on a fresh realm after reset_realm. Empty when
    // the embedder did not opt in.
    std::string host_namespace;
    Context *ruby_context;
    int64_t max_memory;
    int err_reason;
    bool verbose_exceptions;
    std::vector<Callback*> callbacks;
    // Per-realm state. Main realm is id 0; Context#create_realm adds more.
    // active_realm_id selects which Realm the current op targets — cur(*this)
    // returns it. Holds unique_ptr because Realm contains non-movable Persistents.
    std::unordered_map<int32_t, std::unique_ptr<Realm>> realms;
    int32_t active_realm_id;
    int32_t next_realm_id;
    // Promises that rejected with no handler, awaiting the next microtask
    // checkpoint where we fire 'unhandledrejection' on their realm's globalThis
    // (HTML notify-rejected-promises). Paired with the rejecting realm id; a
    // handler added before notification removes the entry.
    std::vector<std::pair<v8::Global<v8::Promise>, int32_t>> pending_rejections;
    // Depth counter incremented while v8_api_callback is on the stack.
    // CreateCodeCache walks live isolate state and corrupts the parser
    // when invoked from within a JS->Ruby->JS frame; see compile()'s
    // `produce_cache` handling.
    int in_callback;
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
    // Snapshot descriptor for this isolate. V8 stores params.snapshot_blob by
    // pointer (Isolate::Initialize keeps it) and re-reads it on every
    // Context::New — including the post-boot ones from create_realm/reset_realm
    // — so it must outlive the isolate. Holding it in State (which owns and
    // disposes the isolate) is required for single_threaded mode, where
    // v8_thread_init returns while the isolate lives on, destroying any stack
    // local. The bytes it points at live in the Context's snapshot Buf, which
    // also outlives the isolate.
    v8::StartupData snapshot_blob{nullptr, 0};
    inline ~State();
};

// The realm the current op targets. active_realm_id is set when entering a
// realm (boot/reset = 0; realm-scoped ops set it to their realm). Phase 1: it
// is always 0, so this is the main realm and behavior is unchanged.
static inline Realm& cur(State& st) { return *st.realms.at(st.active_realm_id); }

namespace {

// Fire 'unhandledrejection' on each pending rejection's realm (HTML notify-
// rejected-promises). Called at the end of embedder-driven microtask
// checkpoints. Defined after restore_realm_locals.
static void notify_unhandled_rejections(State& st);

// deliberately leaked on program exit,
// not safe to destroy after main() returns
v8::Platform *platform;

struct Serialized
{
    uint8_t *data = nullptr;
    size_t   size = 0;

    Serialized(State& st, v8::Local<v8::Value> v)
    {
        v8::ValueSerializer ser(st.isolate);
        ser.WriteHeader();
        if (!ser.WriteValue(st.context, v).FromMaybe(false)) return; // exception pending
        auto pair = ser.Release();
        data = pair.first;
        size = pair.second;
    }

    ~Serialized()
    {
        free(data);
    }
};

bool bubble_up_ruby_exception(State& st, v8::TryCatch *try_catch)
{
    auto exception = try_catch->Exception();
    if (exception.IsEmpty()) return false;
    auto ruby_exception = v8::Local<v8::Value>::New(st.isolate, st.ruby_exception);
    if (ruby_exception.IsEmpty()) return false;
    if (!ruby_exception->SameValue(exception)) return false;
    // signal that the ruby thread should reraise the exception
    // that it caught earlier when executing a js->ruby callback
    uint8_t c = 'e';
    v8_reply(st.ruby_context, &c, 1);
    return true;
}

// throws JS exception on serialization error
bool reply(State& st, v8::Local<v8::Value> v)
{
    v8::TryCatch try_catch(st.isolate);
    {
        Serialized serialized(st, v);
        if (serialized.data) {
            v8_reply(st.ruby_context, serialized.data, serialized.size);
            return true;
        }
    }
    if (!try_catch.CanContinue()) {
        try_catch.ReThrow();
        return false;
    }
    auto recv = v8::Undefined(st.isolate);
    if (!st.safe_context_function->Call(st.safe_context, recv, 1, &v).ToLocal(&v)) {
        try_catch.ReThrow();
        return false;
    }
    Serialized serialized(st, v);
    if (serialized.data) {
        v8_reply(st.ruby_context, serialized.data, serialized.size);
        return true;
    }
    // The filtered value still is not serializable — e.g. it contains a Symbol,
    // which the filter passes through unchanged (it only drops functions).
    // ValueSerializer may signal this by returning false WITHOUT throwing, in
    // which case the bare `return false` below would propagate no exception:
    // this TryCatch would clear it on destruction and the caller would hit
    // `assert(try_catch.HasCaught())`. Maintain the invariant that reply()==false
    // implies a pending, propagated exception — synthesize a clone error when
    // none was thrown — so the caller returns the {"error": ...} response
    // instead of aborting. The "could not be cloned" wording matches the
    // heuristic in the 3-arg reply().
    if (!try_catch.HasCaught()) {
        st.isolate->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8Literal(st.isolate, "value could not be cloned")));
    }
    try_catch.ReThrow();
    return false;
}

bool reply(State& st, v8::Local<v8::Value> result, v8::Local<v8::Value> err)
{
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::Local<v8::Array> response;
    {
        v8::Context::Scope context_scope(st.safe_context);
        response = v8::Array::New(st.isolate, 2);
    }
    // Set() can be preempted by a watchdog/OOM TerminateExecution that lands in
    // the window between the caller's epilogue clearing termination (its
    // IsExecutionTerminating check) and here. Under a pending termination Set()
    // returns Nothing and .Check() would abort the whole process; instead
    // propagate it as a failed reply so the caller's `goto fail` retry loop
    // cancels the termination and replies again. Mirrors v8_compile and the
    // 1-arg reply() above.
    if (response->Set(st.context, 0, result).IsNothing() ||
        response->Set(st.context, 1, err).IsNothing()) {
        try_catch.ReThrow();
        return false;
    }
    if (reply(st, response)) return true;
    if (!try_catch.CanContinue()) { // termination exception?
        try_catch.ReThrow();
        return false;
    }
    v8::String::Utf8Value s(st.isolate, try_catch.Exception());
    const char *message = *s ? *s : "unexpected failure";
    // most serialization errors will be DataCloneErrors but not always
    // DataCloneErrors are not directly detectable so use a heuristic
    if (!strstr(message, "could not be cloned")) {
        try_catch.ReThrow();
        return false;
    }
    // return an {"error": "foo could not be cloned"} object
    v8::Local<v8::Object> error;
    {
        v8::Context::Scope context_scope(st.safe_context);
        error = v8::Object::New(st.isolate);
    }
    auto key = v8::String::NewFromUtf8Literal(st.isolate, "error");
    v8::Local<v8::String> val;
    if (!v8::String::NewFromUtf8(st.isolate, message).ToLocal(&val)) {
        val = v8::String::NewFromUtf8Literal(st.isolate, "unexpected error");
    }
    // Same termination hazard as above: don't .Check() these Sets.
    if (error->Set(st.context, key, val).IsNothing() ||
        response->Set(st.context, 0, error).IsNothing()) {
        try_catch.ReThrow();
        return false;
    }
    if (!reply(st, response)) {
        try_catch.ReThrow();
        return false;
    }
    return true;
}

// for when a reply is not expected to fail because of serialization
// errors but can still fail when preempted by isolate termination;
// temporarily cancels the termination exception so it can send the reply
void reply_retry(State& st, v8::Local<v8::Value> response)
{
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    bool ok = reply(st, response);
    while (!ok) {
        assert(try_catch.HasCaught());
        assert(try_catch.HasTerminated());
        if (!try_catch.HasTerminated()) abort();
        st.isolate->CancelTerminateExecution();
        ok = reply(st, response);
        st.isolate->TerminateExecution();
    }
}

// Reply a RUNTIME_ERROR refusing a realm op (|op| names the method) invoked
// while a JS->Ruby callback is suspended on the stack (in_callback > 0).
// Mutating the realm out from under such a frame corrupts it (reset_realm) or
// aborts when the resumed frame looks the realm up (dispose_realm).
static void refuse_in_callback(State& st, const char *op)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%c%s cannot be called from within a host function callback",
             RUNTIME_ERROR, op);
    v8::Local<v8::String> err;
    if (!v8::String::NewFromUtf8(st.isolate, buf).ToLocal(&err))
        err = v8::String::Empty(st.isolate);
    reply_retry(st, err);
}

v8::Local<v8::Value> sanitize(State& st, v8::Local<v8::Value> v)
{
    // punch through proxies
    while (v->IsProxy()) v = v8::Proxy::Cast(*v)->GetTarget();
    // V8's serializer doesn't accept symbols
    if (v->IsSymbol()) return v8::Symbol::Cast(*v)->Description(st.isolate);
    // TODO(bnoordhuis) replace this hack with something more principled
    if (v->IsFunction()) {
        auto type = v8::NewStringType::kNormal;
        const size_t size = sizeof(js_function_marker) / sizeof(*js_function_marker);
        return v8::String::NewFromTwoByte(st.isolate, js_function_marker, type, size).ToLocalChecked();
    }
    if (v->IsWeakMap() || v->IsWeakSet() || v->IsMapIterator() || v->IsSetIterator()) {
        bool is_key_value;
        v8::Local<v8::Array> array;
        if (v8::Object::Cast(*v)->PreviewEntries(&is_key_value).ToLocal(&array)) {
            return array;
        }
    }
    return v;
}

v8::Local<v8::String> to_error(State& st, v8::TryCatch *try_catch, int cause)
{
    v8::Local<v8::Value> t;
    char buf[1024];

    *buf = '\0';
    if (cause == NO_ERROR) {
        // nothing to do
    } else if (cause == PARSE_ERROR) {
        auto message = try_catch->Message();
        v8::String::Utf8Value s(st.isolate, message->Get());
        v8::String::Utf8Value name(st.isolate, message->GetScriptResourceName());
        if (!*s || !*name) goto fallback;
        auto line = message->GetLineNumber(st.context).FromMaybe(0);
        auto column = message->GetStartColumn(st.context).FromMaybe(0);
        snprintf(buf, sizeof(buf), "%c%s at %s:%d:%d", cause, *s, *name, line, column);
    } else if (try_catch->StackTrace(st.context).ToLocal(&t)) {
        v8::String::Utf8Value s(st.isolate, t);
        if (!*s) goto fallback;
        snprintf(buf, sizeof(buf), "%c%s", cause, *s);
    } else {
    fallback:
        v8::String::Utf8Value s(st.isolate, try_catch->Exception());
        const char *message = *s ? *s : "unexpected failure";
        if (cause == MEMORY_ERROR) message = "out of memory";
        if (cause == TERMINATED_ERROR) message = "terminated";
        snprintf(buf, sizeof(buf), "%c%s", cause, message);
    }
    v8::Local<v8::String> s;
    if (v8::String::NewFromUtf8(st.isolate, buf).ToLocal(&s)) return s;
    return v8::String::Empty(st.isolate);
}

extern "C" void v8_global_init(void)
{
    char *p;
    size_t n;

    v8_get_flags(&p, &n);
    if (p) {
        for (char *s = p; s < p+n; s += 1 + strlen(s)) {
            v8::V8::SetFlagsFromString(s);
        }
        free(p);
    }
    v8::V8::InitializeICU();
    if (single_threaded) {
        platform = v8::platform::NewSingleThreadedDefaultPlatform().release();
    } else {
        platform = v8::platform::NewDefaultPlatform().release();
    }
    v8::V8::InitializePlatform(platform);
    v8::V8::Initialize();
}

void v8_gc_callback(v8::Isolate*, v8::GCType, v8::GCCallbackFlags, void *data)
{
    State& st = *static_cast<State*>(data);
    v8::HeapStatistics s;
    st.isolate->GetHeapStatistics(&s);
    int64_t used_heap_size = static_cast<int64_t>(s.used_heap_size());
    if (used_heap_size > st.max_memory) {
        st.err_reason = MEMORY_ERROR;
        st.isolate->TerminateExecution();
    }
}

// Linear scan of cur(st).modules to map a Local<Module> back to the filename
// captured at compile time. Returns empty string if the module isn't ours
// (shouldn't happen — all live modules come from v8_compile_module /
// load_module_graph). cur(st).modules holds every module for the realm's lifetime
// (reset_realm/teardown is the only reclaim point), so this scan is O(N) in the
// realm's module count; fine for the per-visit-fresh-realm model (N is a single
// page's graph), but a reverse index would be needed for a long-lived realm that
// lazily imports many modules.
static const std::string& module_filename(State& st, v8::Local<v8::Module> mod)
{
    static const std::string empty;
    for (auto& kv : cur(st).modules) {
        auto stored = v8::Local<v8::Module>::New(st.isolate, kv.second->handle);
        if (stored == mod) return kv.second->filename;
    }
    return empty;
}

// RAII marker that the V8 thread is suspended inside a host->Ruby->host
// roundtrip — a host-function call (v8_api_callback), a dynamic import, or a
// module-resolve. While in_callback is nonzero: compile() refuses
// CreateCodeCache (it corrupts V8's parser when run from such a frame), and
// reset_realm refuses to swap the realm out from under the suspended frame.
struct CallbackGuard {
    State &st;
    CallbackGuard(State &s) : st(s) { st.in_callback++; }
    ~CallbackGuard()                { st.in_callback--; }
};

// Forward declarations for the URL-registry module loader (defined below,
// alongside load_module_graph). Used by the registry-backed dynamic import path.
static v8::Local<v8::Module> registry_lookup(State& st, const std::string& url);
static void registry_rollback(State& st, const std::vector<std::string>& urls);
static bool graph_str(State& st, const std::string& s, v8::Local<v8::String>* out);
static bool graph_roundtrip(State& st, char marker, v8::Local<v8::Value> request,
                            v8::Local<v8::Value>* reply_out);
static bool walk_module_graph(State& st, const std::string& entry_url,
                              std::unordered_map<std::string, std::string>& edges,
                              std::vector<std::string>& new_urls,
                              std::unordered_map<std::string, bool>& rejected_by_url);
static v8::MaybeLocal<v8::Module> graph_resolve_callback(
    v8::Local<v8::Context> context, v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_assertions, v8::Local<v8::Module> referrer);

// Opt-in (MINI_RACER_TRACE_MODULES env) stderr tracing of the dynamic-import and
// module-registry boundary — to correlate a real app's imports/registrations
// with leaked handles in a heap snapshot. Inert (one getenv) when unset.
static bool module_trace_on()
{
    static const bool on = (getenv("MINI_RACER_TRACE_MODULES") != nullptr);
    return on;
}

// V8 calls this for every JS `import(...)` expression. We rendezvous to
// Ruby (marker 'd'), expect a fully-instantiated MiniRacerCsim::Module back,
// evaluate it if still pending, then resolve the returned Promise with
// its namespace. The contract requires the embedder to handle compile +
// instantiate + evaluate; Ruby's resolver is responsible for the first
// two, and we run Evaluate here so callers don't have to.
static v8::MaybeLocal<v8::Promise> host_import_module_dynamically_callback(
    v8::Local<v8::Context> context,
    v8::Local<v8::Data> /*host_defined_options*/,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> /*import_attributes*/)
{
    auto isolate = context->GetIsolate();
    State *pst = static_cast<State*>(isolate->GetData(0));
    State& st = *pst;
    // Suspended in a host->Ruby roundtrip for the whole resolver exchange.
    CallbackGuard _guard(st);
    v8::EscapableHandleScope handle_scope(isolate);

    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver))
        return v8::MaybeLocal<v8::Promise>();

    // Single-exit helpers so every error path is one line.
    auto escape = [&] { return handle_scope.Escape(resolver->GetPromise()); };
    auto reject_with_value = [&](v8::Local<v8::Value> reason) {
        (void)resolver->Reject(context, reason);
        return escape();
    };
    // NewFromUtf8Literal returns a Local directly (no allocation Maybe),
    // so error messages are safe under isolate OOM where NewFromUtf8 +
    // ToLocalChecked would CHECK-fail.
    auto reject_with_literal = [&](v8::Local<v8::String> msg) {
        return reject_with_value(v8::Exception::Error(msg));
    };

    v8::Local<v8::Module> module;

    if (module_trace_on()) {
        v8::String::Utf8Value spec(st.isolate, specifier);
        v8::String::Utf8Value ref(st.isolate, resource_name);
        fprintf(stderr, "[mr.dynimport] specifier=%s referrer=%s path=%s\n",
                *spec ? *spec : "?",
                (resource_name->IsString() && *ref) ? *ref : "<none>",
                cur(st).uses_graph_loader ? "registry" : "legacy");
        fflush(stderr);
    }

    if (cur(st).uses_graph_loader) {
        // Registry path: resolve the specifier to a URL via the persisted
        // resolve callback, reuse the registry's Module if the URL was already
        // loaded (the identity fix), else walk + instantiate its subgraph. A
        // local TryCatch turns fetch/resolve/compile failures into a rejected
        // import() promise instead of leaving an exception pending.
        v8::TryCatch tc(st.isolate);
        tc.SetVerbose(st.verbose_exceptions);
        auto reject_pending = [&] {
            v8::Local<v8::Value> reason = tc.HasCaught()
                ? tc.Exception()
                : v8::Local<v8::Value>::Cast(v8::Exception::Error(
                      v8::String::NewFromUtf8Literal(isolate, "dynamic import failed")));
            // Clear so the captured Ruby error (if any) is reported via the
            // promise, not re-raised in the enclosing eval frame.
            st.ruby_exception.Reset();
            return reject_with_value(reason);
        };

        std::string ref_url;
        if (resource_name->IsString()) {
            v8::String::Utf8Value ru(st.isolate, resource_name);
            if (*ru) ref_url.assign(*ru, ru.length());
        }
        // Single-edge resolve batch: [[specifier, referrer_url]].
        v8::Local<v8::Array> edges_arr, pr;
        {
            v8::Context::Scope cs(st.safe_context);
            edges_arr = v8::Array::New(st.isolate, 1);
            pr = v8::Array::New(st.isolate, 2);
        }
        v8::Local<v8::String> refs;
        if (!graph_str(st, ref_url, &refs)) refs = v8::String::Empty(st.isolate);
        // Under the local tc, a watchdog termination makes Set() return
        // Nothing. .Check() would abort; but simply returning would let tc clear
        // the termination on destruction (and reject_pending() would convert it
        // into a normal import rejection, swallowing it). Rethrow past tc so the
        // termination propagates to and terminates the enclosing eval.
        if (pr->Set(context, 0, specifier).IsNothing() ||
            pr->Set(context, 1, refs).IsNothing() ||
            edges_arr->Set(context, 0, pr).IsNothing()) {
            tc.ReThrow();
            return v8::MaybeLocal<v8::Promise>();
        }
        v8::Local<v8::Value> resolved_v;
        if (!graph_roundtrip(st, 'r', edges_arr, &resolved_v)) return reject_pending();
        std::string url;
        if (resolved_v->IsArray()) {
            v8::Local<v8::Value> u;
            if (resolved_v.As<v8::Array>()->Get(context, 0).ToLocal(&u) && u->IsString()) {
                v8::String::Utf8Value uu(st.isolate, u);
                if (*uu) url.assign(*uu, uu.length());
            }
        }
        if (url.empty())
            return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
                "dynamic import specifier could not be resolved to a URL"));

        module = registry_lookup(st, url);
        if (module_trace_on())
            fprintf(stderr, "[mr.dynimport] resolved url=%s registry_%s\n",
                    url.c_str(), module.IsEmpty() ? "miss" : "hit"), fflush(stderr);
        if (module.IsEmpty()) {
            // Miss: load the not-yet-registered subgraph reachable from url,
            // then instantiate (the shared tail below evaluates + resolves). On
            // failure roll back what this walk registered so a retry recompiles
            // cleanly. active_graph is save/restored (a dynamic import may itself
            // fire inside an enclosing load's Evaluate).
            GraphLoad graph;
            std::vector<std::string> new_urls;
            std::unordered_map<std::string, bool> rejected_by_url;
            if (!walk_module_graph(st, url, graph.edges, new_urls, rejected_by_url)) {
                registry_rollback(st, new_urls);
                return reject_pending();
            }
            module = registry_lookup(st, url);
            if (module.IsEmpty()) {
                registry_rollback(st, new_urls);
                return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
                    "dynamic import target could not be fetched"));
            }
            GraphLoad *prev = cur(st).active_graph;
            cur(st).active_graph = &graph;
            v8::Maybe<bool> ok = module->InstantiateModule(st.context, graph_resolve_callback);
            cur(st).active_graph = prev;
            if (ok.IsNothing() || !ok.FromJust()) {
                registry_rollback(st, new_urls);
                return reject_pending();
            }
        }
    } else {
        // Legacy path: the embedder's dynamic_import_resolver returns a
        // fully-instantiated MiniRacerCsim::Module (looked up by handle id).
        v8::Local<v8::Array> request;
        {
            v8::Context::Scope context_scope(st.safe_context);
            request = v8::Array::New(st.isolate, 2);
        }
        // No local TryCatch on this legacy branch, so a watchdog termination
        // here (Set() -> Nothing) propagates if we return an empty MaybeLocal;
        // .Check() would abort instead.
        if (request->Set(context, 0, specifier).IsNothing()) return v8::MaybeLocal<v8::Promise>();
        // resource_name is the referrer's filename for module-initiated imports,
        // or the script filename for eval-initiated ones. May be Undefined for
        // ad-hoc compilations; coerce to empty string in that case.
        v8::Local<v8::Value> ref = resource_name->IsString()
            ? resource_name
            : v8::Local<v8::Value>::Cast(v8::String::Empty(st.isolate));
        if (request->Set(context, 1, ref).IsNothing()) return v8::MaybeLocal<v8::Promise>();

        {
            Serialized serialized(st, request);
            if (!serialized.data)
                return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
                    "could not serialize dynamic import request"));
            uint8_t marker = 'd';
            v8_reply(st.ruby_context, &marker, 1);
            v8_reply(st.ruby_context, serialized.data, serialized.size);
        }

        const uint8_t *p;
        size_t n;
        for (;;) {
            v8_roundtrip(st.ruby_context, &p, &n);
            if (*p == 'd') break;
            if (*p == 'e') {
                v8::Local<v8::String> message;
                auto type = v8::NewStringType::kNormal;
                if (!v8::String::NewFromOneByte(st.isolate, p+1, type, n-1).ToLocal(&message))
                    message = v8::String::NewFromUtf8Literal(st.isolate, "Ruby exception");
                return reject_with_literal(message);
            }
            v8_dispatch(st.ruby_context);
        }

        v8::ValueDeserializer des(st.isolate, p+1, n-1);
        des.ReadHeader(st.context).Check();
        v8::Local<v8::Value> id_v;
        int32_t id;
        if (!des.ReadValue(st.context).ToLocal(&id_v) ||
            !id_v->Int32Value(st.context).To(&id))
            return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
                "dynamic import reply could not be decoded"));
        auto it = cur(st).modules.find(id);
        if (it == cur(st).modules.end())
            return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
                "dynamic import resolver returned a handle unknown to this Context"));
        module = v8::Local<v8::Module>::New(st.isolate, it->second->handle);
        if (module_trace_on())
            fprintf(stderr, "[mr.dynimport] legacy resolver returned id=%d url=%s\n",
                    id, it->second->filename.c_str()), fflush(stderr);
    }

    auto status = module->GetStatus();
    // The Ruby resolver must hand back a Module that's at least instantiated;
    // auto-instantiating here is impossible because there's no per-call
    // resolver block to recurse through.
    if (status < v8::Module::kInstantiated)
        return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
            "dynamic import resolver returned an uninstantiated Module"));
    if (status == v8::Module::kErrored)
        return reject_with_value(module->GetException());
    // kEvaluating means re-entry during cyclic dynamic import: V8 would
    // give us a TDZ-laden namespace whose bindings throw ReferenceError.
    // Spec-correct handling is to settle after the in-flight Evaluate
    // completes, which requires TLA support; reject explicitly for now.
    if (status == v8::Module::kEvaluating)
        return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
            "dynamic import target is mid-evaluation (cyclic dynamic import)"));
    if (status == v8::Module::kInstantiated) {
        v8::TryCatch try_catch(st.isolate);
        try_catch.SetVerbose(st.verbose_exceptions);
        v8::Local<v8::Value> eval_result;
        if (!module->Evaluate(context).ToLocal(&eval_result)) {
            // Termination set the empty MaybeLocal without throwing — let
            // the surrounding eval frame surface it instead of swallowing.
            if (isolate->IsExecutionTerminating())
                return v8::MaybeLocal<v8::Promise>();
            return reject_with_value(try_catch.HasCaught()
                ? try_catch.Exception()
                : v8::Local<v8::Value>::Cast(v8::Undefined(isolate)));
        }
        // Drain so synchronously-scheduled microtasks (e.g. the dep body's
        // own Promise.resolve().then) settle before we inspect promise state;
        // matches v8_evaluate_module.
        isolate->PerformMicrotaskCheckpoint();
        if (eval_result->IsPromise()) {
            auto promise = eval_result.As<v8::Promise>();
            if (promise->State() == v8::Promise::kRejected)
                return reject_with_value(promise->Result());
            if (promise->State() == v8::Promise::kPending)
                return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
                    "dynamic import target has top-level await (not supported)"));
        }
    } else if (status == v8::Module::kEvaluated && module->IsGraphAsync()) {
        // An already-evaluated module handed back by a registry hit (or the
        // resolver). The kInstantiated branch above only confirmed settlement
        // for the module it just evaluated; a previously-evaluated async module
        // may still have a pending top-level await whose TDZ namespace would
        // fatally abort the process when serialized. Refuse it.
        return reject_with_literal(v8::String::NewFromUtf8Literal(isolate,
            "dynamic import target uses top-level await (not supported)"));
    }

    (void)resolver->Resolve(context, module->GetModuleNamespace());
    return escape();
}

// V8 calls this the first time JS reads `import.meta` for a module.
// Populate the `url` property with the filename passed to compile_module
// — needed for relative resolution helpers like `new URL(spec, import.meta.url)`.
// Graph/dynamic-import modules are registered in cur(st).modules with filename=url,
// so module_filename resolves them too.
static void init_import_meta_object(v8::Local<v8::Context> context,
                                    v8::Local<v8::Module> module,
                                    v8::Local<v8::Object> meta)
{
    auto isolate = context->GetIsolate();
    // module_filename() materializes a Local<Module> per entry while scanning;
    // give them a scope to reclaim instead of piling onto the caller's.
    v8::HandleScope handle_scope(isolate);
    State *pst = static_cast<State*>(isolate->GetData(0));
    const std::string& filename = module_filename(*pst, module);
    // Pass the byte length explicitly: filenames may contain embedded NULs,
    // and NewFromUtf8 without a length argument truncates at the first NUL.
    v8::Local<v8::String> name;
    auto type = v8::NewStringType::kNormal;
    if (!v8::String::NewFromUtf8(isolate, filename.data(), type,
                                 static_cast<int>(filename.size())).ToLocal(&name))
        return;
    auto key = v8::String::NewFromUtf8Literal(isolate, "url");
    // Do not Check() — a user-installed setter on Object.prototype.url
    // would throw, and Check() would abort the process. Letting the
    // Maybe<bool> drop surfaces the failure as a JS exception via the
    // surrounding TryCatch frame.
    (void)meta->Set(context, key, name);
}

// Native, rendezvous-free microtask checkpoint. When the embedder opts in via
// Context.new(host_namespace:), it is hung off the host namespace as
// <namespace>.drainMicrotasks(). Unlike Context#perform_microtask_checkpoint
// (dispatch tag 'M') this runs inline on the isolate thread and never
// round-trips through the Ruby<->V8 rendezvous, so JS can drain the queue
// mid-execution -- e.g. between synchronous dispatchEvent listeners -- for
// ~sub-microsecond cost. It mirrors v8_perform_microtask_checkpoint but
// without the reply, and deliberately leaves any termination active so the
// enclosing v8_call/v8_eval frame surfaces OOM (v8_gc_callback) or watchdog
// termination to Ruby.
void v8_drain_microtasks_callback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto ext = v8::External::Cast(*info.Data());
    State& st = *static_cast<State*>(ext->Value());
    // Do *not* take a v8::Locker here: in single-threaded mode V8 already holds
    // the isolate on this (the Ruby) thread, so locking would deadlock.
    //
    // An uncaught exception thrown by a drained microtask is routed by V8 to
    // its message/unhandled-rejection handlers, not propagated out of
    // PerformCheckpoint, so this TryCatch normally catches nothing; it exists
    // only to mirror v8_perform_microtask_checkpoint and honor verbose_exceptions.
    // It must not (and does not) clear a pending termination.
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    // PerformCheckpoint is a guarded no-op when the microtask depth is > 0, so
    // it is safe to call mid-execution and never force-nests microtask runs.
    v8::MicrotasksScope::PerformCheckpoint(st.isolate);
    notify_unhandled_rejections(st);
    info.GetReturnValue().SetUndefined();
}

// <host_namespace>.realmGlobal(id): returns the globalThis of realm `id` as a
// LIVE V8 object in the calling realm (same isolate, not a copy), or undefined
// for an unknown realm. Hung off the host namespace (opt-in via
// Context.new(host_namespace:)) rather than a bare global, so globalThis stays
// unpolluted. Because all realms share one security token (see install_realm),
// the caller can read/write the returned global's properties — this is how csim
// wires frames[i] / iframe.contentWindow to the right realm. Cross-realm object
// identity holds: mutating a property here is visible in the target realm.
void v8_realm_global_callback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto isolate = info.GetIsolate();
    State& st = *static_cast<State*>(v8::External::Cast(*info.Data())->Value());
    auto context = isolate->GetCurrentContext();
    int32_t id;
    if (info.Length() < 1 || !info[0]->Int32Value(context).To(&id)) {
        info.GetReturnValue().SetUndefined();
        return;
    }
    auto it = st.realms.find(id);
    if (it == st.realms.end()) {
        info.GetReturnValue().SetUndefined();
        return;
    }
    auto target = v8::Local<v8::Context>::New(isolate, it->second->persistent_context);
    if (target.IsEmpty()) {
        info.GetReturnValue().SetUndefined();
        return;
    }
    info.GetReturnValue().Set(target->Global());
}

// Builds a fresh user realm (plus the companion safe context), wires the
// safe-context marshalling helper against the new global, installs the opt-in
// host namespace, and re-binds any previously attached host functions. On
// success it atomically commits the new realm into st.persistent_* (releasing
// the previous one) and returns true. On any failure it touches none of the
// persistents — the previous realm stays intact — and returns false with an
// Realm id stored in each user context's embedder data (slot 1) at build time,
// so a promise's creation context maps back to its realm in O(1).
static const int kRealmIdEmbedderSlot = 1;
static int32_t realm_id_of_context(v8::Local<v8::Context> ctx)
{
    if (ctx.IsEmpty()) return -1;
    auto v = ctx->GetEmbedderData(kRealmIdEmbedderSlot);
    if (v.IsEmpty() || !v->IsInt32()) return -1;
    return v.As<v8::Int32>()->Value();
}

// <host_namespace>.realmOf(fn): returns the realm id where `fn` (any
// object/function) was created — its [[Realm]] / creation context — or
// undefined if unknown. This is the realm WebIDL's "invoke a callback function"
// reports errors against (e.g. a setTimeout callback's uncaught throw is
// reported on the realm that *created* the callback, not the scheduling realm
// nor the thrown Error's realm). csim uses it to dispatch an ErrorEvent on
// <ns>.realmGlobal(<ns>.realmOf(cb)) from its own per-callback try/catch.
void v8_realm_of_callback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto isolate = info.GetIsolate();
    if (info.Length() < 1 || !info[0]->IsObject()) {
        info.GetReturnValue().SetUndefined();
        return;
    }
    v8::Local<v8::Context> ctx;
    if (!info[0].As<v8::Object>()->GetCreationContext(isolate).ToLocal(&ctx)) {
        info.GetReturnValue().SetUndefined();
        return;
    }
    int32_t rid = realm_id_of_context(ctx);
    if (rid < 0) {
        info.GetReturnValue().SetUndefined();
        return;
    }
    info.GetReturnValue().Set(v8::Integer::New(isolate, rid));
}

// <host_namespace>.onUnhandledRejection(fn): register fn as the calling realm's
// handler for promises that reject with no handler. notify_unhandled_rejections
// calls it with (reason, promise) at the next microtask checkpoint, entered in
// the rejecting promise's realm (HTML notify-rejected-promises). The handler is
// stored per realm (keyed by the calling context's realm id) instead of as a
// bare globalThis property, so globalThis stays unpolluted. Passing a non-
// function clears the handler.
void v8_set_unhandled_rejection_handler(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto isolate = info.GetIsolate();
    State& st = *static_cast<State*>(isolate->GetData(0));
    int32_t rid = realm_id_of_context(isolate->GetCurrentContext());
    auto it = st.realms.find(rid);
    if (it == st.realms.end())
        return; // unknown realm: no-op
    if (info.Length() >= 1 && info[0]->IsFunction())
        it->second->unhandled_rejection_handler.Reset(isolate, info[0].As<v8::Function>());
    else
        it->second->unhandled_rejection_handler.Reset();
}

// V8 calls this when a promise's rejection state changes. We implement the
// HTML "notify rejected promises" bookkeeping: queue promises that reject with
// no handler (tagged with the realm they were created in), and drop them again
// if a handler is attached before the next checkpoint. The actual
// 'unhandledrejection' dispatch happens in notify_unhandled_rejections.
void promise_reject_callback(v8::PromiseRejectMessage msg)
{
    auto promise = msg.GetPromise();
    auto isolate = promise->GetIsolate();
    State& st = *static_cast<State*>(isolate->GetData(0));
    switch (msg.GetEvent()) {
    case v8::kPromiseRejectWithNoHandler: {
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> ctx;
        if (!promise->GetCreationContext(isolate).ToLocal(&ctx)) return;
        int32_t rid = realm_id_of_context(ctx);
        if (rid < 0) return; // not one of our realms
        st.pending_rejections.emplace_back(v8::Global<v8::Promise>(isolate, promise), rid);
        break;
    }
    case v8::kPromiseHandlerAddedAfterReject: {
        v8::HandleScope handle_scope(isolate);
        for (auto it = st.pending_rejections.begin(); it != st.pending_rejections.end();) {
            if (v8::Local<v8::Promise>::New(isolate, it->first) == promise)
                it = st.pending_rejections.erase(it);
            else
                ++it;
        }
        break;
    }
    default:
        break; // kPromiseRejectAfterResolved / kPromiseResolveAfterResolved
    }
}

// exception pending in the caller's TryCatch. Defined after v8_api_callback
// (which the re-bind needs). Assumes the isolate is entered by the caller.
static bool install_realm(State& st);

extern "C" State *v8_thread_init(Context *c, const uint8_t *snapshot_buf,
                                 size_t snapshot_len, int64_t max_memory,
                                 int verbose_exceptions,
                                 const char *host_namespace)
{
    State *pst = new State{};
    State& st = *pst;
    st.verbose_exceptions = (verbose_exceptions != 0);
    st.ruby_context = c;
    st.allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = st.allocator.get();
    if (snapshot_len) {
        // st.snapshot_blob (not a stack local) so it outlives this frame: in
        // single_threaded mode v8_thread_init returns while the isolate lives,
        // and V8 re-reads params.snapshot_blob on every later Context::New.
        st.snapshot_blob.data = reinterpret_cast<const char*>(snapshot_buf);
        st.snapshot_blob.raw_size = snapshot_len;
        params.snapshot_blob = &st.snapshot_blob;
    }
    st.isolate = v8::Isolate::New(params);
    // Slot 0 lets v8 callbacks that don't take embedder data (notably
    // Module::InstantiateModule's ResolveCallback) recover State.
    st.isolate->SetData(0, pst);
    // Populate `import.meta.url` with the filename passed to compile_module.
    st.isolate->SetHostInitializeImportMetaObjectCallback(init_import_meta_object);
    // Dispatch JS `import(...)` expressions to Ruby via marker 'd'.
    st.isolate->SetHostImportModuleDynamicallyCallback(
        host_import_module_dynamically_callback);
    // Track unhandled promise rejections so we can fire 'unhandledrejection' on
    // the rejecting realm's globalThis at the next microtask checkpoint.
    st.isolate->SetPromiseRejectCallback(promise_reject_callback);
    st.max_memory = max_memory;
    if (st.max_memory > 0)
        st.isolate->AddGCEpilogueCallback(v8_gc_callback, pst);
    {
        v8::Locker locker(st.isolate);
        v8::Isolate::Scope isolate_scope(st.isolate);
        v8::HandleScope handle_scope(st.isolate);
        st.host_namespace = host_namespace ? host_namespace : "";
        // Create the main realm (id 0) before building it; cur(st) targets it.
        st.realms[0] = std::make_unique<Realm>();
        st.active_realm_id = 0;
        st.next_realm_id = 1;
        // Build the user/safe realm and root it in st.persistent_*. The Local
        // members are not kept alive past here; each request re-derives them
        // from the persistents (see v8_threaded_enter / v8_single_threaded_enter).
        // On a fresh isolate this only fails under catastrophic conditions (it
        // used to be a hard CHECK), so treat boot failure as fatal.
        if (!install_realm(st)) {
            fprintf(stderr, "mini_racer: failed to initialize the V8 realm\n");
            fflush(stderr);
            abort();
        }
        if (single_threaded)
            return pst; // intentionally returning early and keeping alive
        v8_thread_main(c, pst);
    }
    delete pst;
    return nullptr;
}

void v8_api_callback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto ext = v8::External::Cast(*info.Data());
    Callback *cb = static_cast<Callback*>(ext->Value());
    State& st = *cb->st;
    // Suspended in a host->Ruby roundtrip for the whole callback exchange.
    CallbackGuard _guard(st);
    v8::Local<v8::Array> request;
    {
        v8::Context::Scope context_scope(st.safe_context);
        request = v8::Array::New(st.isolate, 1 + info.Length());
    }
    for (int i = 0, n = info.Length(); i < n; i++) {
        // A watchdog/OOM termination during the callback makes Set() return
        // Nothing; .Check() would abort. Return with the (termination) exception
        // pending, exactly like the serialization-failure path just below.
        if (request->Set(st.context, i, sanitize(st, info[i])).IsNothing()) return;
    }
    auto id = v8::Int32::New(st.isolate, cb->id);
    if (request->Set(st.context, info.Length(), id).IsNothing()) return; // callback id
    {
        Serialized serialized(st, request);
        if (!serialized.data) return; // exception pending
        uint8_t marker = 'c'; // callback marker
        v8_reply(st.ruby_context, &marker, 1);
        v8_reply(st.ruby_context, serialized.data, serialized.size);
    }
    const uint8_t *p;
    size_t n;
    for (;;) {
        v8_roundtrip(st.ruby_context, &p, &n);
        if (*p == 'c') // callback reply
            break;
        if (*p == 'e') { // ruby exception pending
            v8::Local<v8::String> message;
            auto type = v8::NewStringType::kNormal;
            if (!v8::String::NewFromOneByte(st.isolate, p+1, type, n-1).ToLocal(&message)) {
                message = v8::String::NewFromUtf8Literal(st.isolate, "Ruby exception");
            }
            auto exception = v8::Exception::Error(message);
            st.ruby_exception.Reset(st.isolate, exception);
            st.isolate->ThrowException(exception);
            return;
        }
        v8_dispatch(st.ruby_context);
    }
    v8::ValueDeserializer des(st.isolate, p+1, n-1);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    if (!des.ReadValue(st.context).ToLocal(&result)) return; // exception pending
    info.GetReturnValue().Set(result);
}

// Binds cb's JS shim onto the current user global at cb->name, creating any
// intermediate objects for dotted "foo.bar.baz" paths. The Callback must
// already be registered in st.callbacks (it owns the External we hand to v8).
// Returns false with an exception pending in the caller's TryCatch on failure.
static bool bind_callback(State& st, Callback *cb)
{
    auto ext = v8::External::New(st.isolate, cb);
    v8::Local<v8::Function> function;
    if (!v8::Function::New(st.context, v8_api_callback, ext).ToLocal(&function))
        return false;
    auto type = v8::NewStringType::kNormal;
    v8::Local<v8::Object> obj = st.context->Global();
    v8::Local<v8::String> key;
    for (const char *p = cb->name.c_str();;) {
        size_t len = strcspn(p, ".");
        if (!v8::String::NewFromUtf8(st.isolate, p, type, len).ToLocal(&key))
            return false;
        if (p[len] == '\0') break;
        p += len + 1;
        v8::Local<v8::Value> val;
        if (!obj->Get(st.context, key).ToLocal(&val)) return false;
        if (!val->IsObject() && !val->IsFunction()) {
            val = v8::Object::New(st.isolate);
            if (!obj->Set(st.context, key, val).FromMaybe(false)) return false;
        }
        obj = val.As<v8::Object>();
    }
    return obj->Set(st.context, key, function).FromMaybe(false);
}

// Re-derive the per-request realm Locals from the canonical persistents, and
// drop them again. Kept in one place so every entry point lists the same three
// members in the same order (v8_threaded_enter, v8_single_threaded_enter,
// v8_reset_realm).
static void restore_realm_locals(State& st)
{
    Realm& r = cur(st);
    st.safe_context_function = v8::Local<v8::Function>::New(st.isolate, r.persistent_safe_context_function);
    st.safe_context = v8::Local<v8::Context>::New(st.isolate, r.persistent_safe_context);
    st.context = v8::Local<v8::Context>::New(st.isolate, r.persistent_context);
}

static void clear_realm_locals(State& st)
{
    st.context = v8::Local<v8::Context>();
    st.safe_context = v8::Local<v8::Context>();
    st.safe_context_function = v8::Local<v8::Function>();
}

// HTML notify-rejected-promises: for each promise that rejected with no handler
// since the last checkpoint, enter its realm and call that realm's handler
// (registered via <host_namespace>.onUnhandledRejection) with (reason, promise)
// if one was set (csim turns that into a PromiseRejectionEvent so
// addEventListener('unhandledrejection') fires natively in the right realm). The
// list is snapshotted and cleared first so rejections triggered by a handler
// queue for the next checkpoint instead of looping. A realm disposed in the
// meantime is skipped.
static void notify_unhandled_rejections(State& st)
{
    if (st.pending_rejections.empty())
        return;
    std::vector<std::pair<v8::Global<v8::Promise>, int32_t>> pending;
    pending.swap(st.pending_rejections);
    // Preserve the caller's active-realm Locals (restore by assignment, never
    // re-derive into this scope — see v8_create_realm).
    v8::Local<v8::Context> saved_context = st.context;
    v8::Local<v8::Context> saved_safe_context = st.safe_context;
    v8::Local<v8::Function> saved_safe_context_function = st.safe_context_function;
    int32_t prev = st.active_realm_id;
    for (auto& pr : pending) {
        auto it = st.realms.find(pr.second);
        if (it == st.realms.end())
            continue; // realm disposed since the rejection
        v8::HandleScope handle_scope(st.isolate);
        auto context = v8::Local<v8::Context>::New(st.isolate, it->second->persistent_context);
        if (context.IsEmpty())
            continue;
        if (it->second->unhandled_rejection_handler.IsEmpty())
            continue; // realm registered no onUnhandledRejection handler
        st.active_realm_id = pr.second;
        restore_realm_locals(st);
        v8::Context::Scope context_scope(context);
        auto global = context->Global();
        auto hook = v8::Local<v8::Function>::New(st.isolate, it->second->unhandled_rejection_handler);
        auto promise = v8::Local<v8::Promise>::New(st.isolate, pr.first);
        v8::TryCatch try_catch(st.isolate); // swallow errors from the handler itself
        v8::Local<v8::Value> args[2] = { promise->Result(), promise };
        (void)hook->Call(context, global, 2, args);
    }
    st.active_realm_id = prev;
    st.context = saved_context;
    st.safe_context = saved_safe_context;
    st.safe_context_function = saved_safe_context_function;
}

// See the forward declaration above v8_thread_init for the contract. All
// build-time handles live in a private HandleScope. Nothing is committed until
// the realm is fully built (including every host-fn re-bind), so a failure
// midway — e.g. the isolate is terminating from a watchdog/OOM — leaves the
// previous realm untouched instead of CHECK-crashing the process.
static bool install_realm(State& st)
{
    State *pst = &st;
    v8::HandleScope handle_scope(st.isolate);
    v8::Local<v8::Context> safe_context = v8::Context::New(st.isolate);
    v8::Local<v8::Context> context = v8::Context::New(st.isolate);
    if (safe_context.IsEmpty() || context.IsEmpty()) return false;
    // Give every realm the same security token (captured from the first one)
    // so per-frame realms can access each other's globals. Set before the safe
    // grant below so it reads the shared token via context->GetSecurityToken().
    if (st.shared_security_token.IsEmpty())
        st.shared_security_token.Reset(st.isolate, context->GetSecurityToken());
    context->SetSecurityToken(v8::Local<v8::Value>::New(st.isolate, st.shared_security_token));
    // Tag the user context with its realm id so the promise-reject callback can
    // map a rejecting promise back to its realm in O(1).
    context->SetEmbedderData(kRealmIdEmbedderSlot, v8::Integer::New(st.isolate, st.active_realm_id));
    v8::Local<v8::Function> safe_context_function;
    {
        v8::Context::Scope safe_scope(safe_context);
        auto source = v8::String::NewFromUtf8Literal(st.isolate, safe_context_script_source);
        auto filename = v8::String::NewFromUtf8Literal(st.isolate, "safe_context_script.js");
        v8::ScriptOrigin origin(filename);
        v8::Local<v8::Script> script;
        if (!v8::Script::Compile(safe_context, source, &origin).ToLocal(&script))
            return false;
        v8::Local<v8::Value> function_v;
        if (!script->Run(safe_context).ToLocal(&function_v)) return false;
        auto function = v8::Function::Cast(*function_v);
        auto recv = v8::Undefined(st.isolate);
        v8::Local<v8::Value> arg = context->Global();
        // grant the safe context access to the user context's globalThis
        safe_context->SetSecurityToken(context->GetSecurityToken());
        v8::Local<v8::Value> ret;
        bool ok = function->Call(safe_context, recv, 1, &arg).ToLocal(&ret);
        // revoke access again now that the script did its one-time setup
        safe_context->UseDefaultSecurityToken();
        if (!ok) return false;
        safe_context_function = v8::Local<v8::Function>::Cast(ret);
    }
    v8::Context::Scope context_scope(context);
    // If the embedder opted in via Context.new(host_namespace:), install a
    // single host-namespace object (in the spirit of Deno's `Deno` / Bun's
    // `Bun`) under that global name and hang EVERY native JS helper off it:
    // drainMicrotasks(), realmGlobal(id), realmOf(fn), onUnhandledRejection(fn).
    // Keeping them on one opt-in object (rather than bare __mr_* globals) means
    // globalThis pollution is decided once, by the opt-in — not relitigated per
    // feature. The object closes over native code pointers so it cannot live in
    // the (de)serialized snapshot; it is installed here on every fresh realm.
    // The namespace is non-enumerable on globalThis (out of Object.keys/for-in);
    // its methods are ordinary properties so they remain discoverable.
    //
    // Consequence: the JS-side realm-reflection helpers (realmGlobal/realmOf)
    // and per-realm unhandled-rejection delivery require host_namespace. Realms
    // themselves do NOT — Context#create_realm + Realm#eval/call/attach work
    // without it (isolated realms driven from Ruby); only cross-realm wiring *in
    // JS* needs these helpers, which is why they live on the namespace.
    if (!st.host_namespace.empty()) {
        v8::Local<v8::String> ns_name;
        if (!v8::String::NewFromUtf8(st.isolate, st.host_namespace.c_str()).ToLocal(&ns_name))
            return false;
        auto ns = v8::Object::New(st.isolate);
        auto data = v8::External::New(st.isolate, pst);
        v8::Local<v8::Function> drain, rg, ro, onrej;
        // drainMicrotasks + realmGlobal read State via info.Data() (the External);
        // realmOf + onUnhandledRejection take none (onUnhandledRejection uses
        // isolate->GetData(0)), so they are created without data.
        if (!v8::Function::New(context, v8_drain_microtasks_callback, data).ToLocal(&drain)) return false;
        if (!v8::Function::New(context, v8_realm_global_callback, data).ToLocal(&rg)) return false;
        if (!v8::Function::New(context, v8_realm_of_callback).ToLocal(&ro)) return false;
        if (!v8::Function::New(context, v8_set_unhandled_rejection_handler).ToLocal(&onrej)) return false;
        if (!ns->Set(context, v8::String::NewFromUtf8Literal(st.isolate, "drainMicrotasks"), drain).FromMaybe(false)) return false;
        if (!ns->Set(context, v8::String::NewFromUtf8Literal(st.isolate, "realmGlobal"), rg).FromMaybe(false)) return false;
        if (!ns->Set(context, v8::String::NewFromUtf8Literal(st.isolate, "realmOf"), ro).FromMaybe(false)) return false;
        if (!ns->Set(context, v8::String::NewFromUtf8Literal(st.isolate, "onUnhandledRejection"), onrej).FromMaybe(false)) return false;
        if (!context->Global()->DefineOwnProperty(context, ns_name, ns, v8::DontEnum).FromMaybe(false))
            return false;
    }
    // Re-attach host functions onto the fresh global. Empty at boot; populated
    // when install_realm runs from v8_reset_realm. bind_callback reads st.context,
    // so point the members at the new realm for the duration of the loop, and
    // make the whole rebuild atomic: if any function cannot be re-bound (e.g. a
    // dotted path now collides with a snapshot global) the reset fails as a whole
    // rather than silently dropping a host function.
    st.context = context;
    st.safe_context = safe_context;
    st.safe_context_function = safe_context_function;
    for (Callback *cb : st.callbacks) {
        if (!bind_callback(st, cb)) {
            clear_realm_locals(st);
            return false;
        }
    }
    // Commit: root the new realm and release the previous one (Reset replaces
    // the old handle). The Local members dangle once handle_scope unwinds, so
    // clear them; the next per-request restore re-derives them.
    Realm& r = cur(st);
    r.persistent_safe_context_function.Reset(st.isolate, safe_context_function);
    r.persistent_safe_context.Reset(st.isolate, safe_context);
    r.persistent_context.Reset(st.isolate, context);
    clear_realm_locals(st);
    return true;
}

// response is err or empty string
extern "C" void v8_attach(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request; // [name, id]
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> name_v;
        if (!request->Get(st.context, 0).ToLocal(&name_v)) goto fail;
        v8::Local<v8::Value> id_v;
        if (!request->Get(st.context, 1).ToLocal(&id_v)) goto fail;
        v8::Local<v8::String> name;
        if (!name_v->ToString(st.context).ToLocal(&name)) goto fail;
        int32_t id;
        if (!id_v->Int32Value(st.context).To(&id)) goto fail;
        // support foo.bar.baz paths
        v8::String::Utf8Value path(st.isolate, name);
        if (!*path) goto fail;
        // The Callback owns its name so reset_realm can re-bind it later. Only
        // register it in st.callbacks (which outlives realm swaps and drives the
        // re-bind) after the bind succeeds, so a failed attach is not resurrected
        // by a later reset_realm. Freed in ~State() once registered.
        Callback *cb = new Callback{pst, id, std::string(*path)};
        if (!bind_callback(st, cb)) {
            delete cb;
            goto fail;
        }
        st.callbacks.push_back(cb);
    }
    cause = NO_ERROR;
fail:
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    auto err = to_error(st, &try_catch, cause);
    reply_retry(st, err);
}

// response is errback [result, err] array
extern "C" void v8_call(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    std::vector<v8::Local<v8::Value>> args;
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request;
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> name_v;
        if (!request->Get(st.context, 0).ToLocal(&name_v)) goto fail;
        v8::Local<v8::String> name;
        if (!name_v->ToString(st.context).ToLocal(&name)) goto fail;
        cause = RUNTIME_ERROR;
        // support foo.bar.baz paths
        v8::String::Utf8Value path(st.isolate, name);
        if (!*path) goto fail;
        v8::Local<v8::Object> obj = st.context->Global();
        v8::Local<v8::String> key;
        for (const char *p = *path;;) {
            size_t n = strcspn(p, ".");
            auto type = v8::NewStringType::kNormal;
            if (!v8::String::NewFromUtf8(st.isolate, p, type, n).ToLocal(&key)) goto fail;
            if (p[n] == '\0') break;
            p += n + 1;
            v8::Local<v8::Value> val;
            if (!obj->Get(st.context, key).ToLocal(&val)) goto fail;
            if (!val->ToObject(st.context).ToLocal(&obj)) goto fail;
        }
        v8::Local<v8::Value> function_v;
        if (!obj->Get(st.context, key).ToLocal(&function_v)) goto fail;
        if (!function_v->IsFunction()) {
            // XXX it's technically possible for |function_v| to be a callable
            // object but those are effectively extinct; regexp objects used
            // to be callable but not anymore
            auto message = v8::String::NewFromUtf8Literal(st.isolate, "not a function");
            auto exception = v8::Exception::TypeError(message);
            st.isolate->ThrowException(exception);
            goto fail;
        }
        auto function = v8::Function::Cast(*function_v);
        assert(request->IsArray());
        int n = v8::Array::Cast(*request)->Length();
        for (int i = 1; i < n; i++) {
            v8::Local<v8::Value> val;
            if (!request->Get(st.context, i).ToLocal(&val)) goto fail;
            args.push_back(val);
        }
        auto maybe_result_v = function->Call(st.context, obj, args.size(), args.data());
        v8::Local<v8::Value> result_v;
        if (!maybe_result_v.ToLocal(&result_v)) goto fail;
        result = sanitize(st, result_v);
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

// response is errback [result, err] array
extern "C" void v8_eval(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request; // [filename, source]
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> filename;
        if (!request->Get(st.context, 0).ToLocal(&filename)) goto fail;
        v8::Local<v8::Value> source_v;
        if (!request->Get(st.context, 1).ToLocal(&source_v)) goto fail;
        v8::Local<v8::String> source;
        if (!source_v->ToString(st.context).ToLocal(&source)) goto fail;
        v8::ScriptOrigin origin(filename);
        v8::Local<v8::Script> script;
        cause = PARSE_ERROR;
        if (!v8::Script::Compile(st.context, source, &origin).ToLocal(&script)) goto fail;
        v8::Local<v8::Value> result_v;
        cause = RUNTIME_ERROR;
        auto maybe_result_v = script->Run(st.context);
        if (!maybe_result_v.ToLocal(&result_v)) goto fail;
        result = sanitize(st, result_v);
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

// Pulls a Module handle id out of the request, looks it up in cur(st).modules,
// and stores the Local in *out. On miss, sets *cause = RUNTIME_ERROR and
// throws a V8 exception; on deserialization failure, leaves *cause alone
// and lets the standard fail-path handler take over. Returns false in
// either failure case so callers can `goto fail` consistently.
static bool module_from_request(State& st,
                                v8::ValueDeserializer& des,
                                v8::Local<v8::Module>* out,
                                int* cause)
{
    v8::Local<v8::Value> id_v;
    if (!des.ReadValue(st.context).ToLocal(&id_v)) return false;
    int32_t id;
    if (!id_v->Int32Value(st.context).To(&id)) return false;
    auto it = cur(st).modules.find(id);
    if (it == cur(st).modules.end()) {
        *cause = RUNTIME_ERROR;
        auto msg = v8::String::NewFromUtf8Literal(st.isolate, "no such module handle");
        st.isolate->ThrowException(v8::Exception::Error(msg));
        return false;
    }
    *out = v8::Local<v8::Module>::New(st.isolate, it->second->handle);
    return true;
}

// request: [filename, source]
// response: errback [handle_id:Int32, err]
//
// Parses |source| as an ES module. handle_id keys cur(st).modules for later
// v8_instantiate_module / v8_evaluate_module / v8_module_namespace /
// v8_dispose_module. Imports declared by the module are not resolved here
// — that happens in v8_instantiate_module via a Ruby-provided resolver.
extern "C" void v8_compile_module(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Array> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request;
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> filename;
        if (!request->Get(st.context, 0).ToLocal(&filename)) goto fail;
        v8::Local<v8::Value> source_v;
        if (!request->Get(st.context, 1).ToLocal(&source_v)) goto fail;
        v8::Local<v8::Value> cached_v;
        if (!request->Get(st.context, 2).ToLocal(&cached_v)) goto fail;
        v8::Local<v8::Value> produce_v;
        if (!request->Get(st.context, 3).ToLocal(&produce_v)) goto fail;
        bool produce_cache = produce_v->BooleanValue(st.isolate);
        v8::Local<v8::String> source;
        if (!source_v->ToString(st.context).ToLocal(&source)) goto fail;

        if (produce_cache && st.in_callback > 0) {
            cause = RUNTIME_ERROR;
            auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                "produce_cache: true is unsafe inside a host-function callback "
                "(V8 CreateCodeCache corrupts parser state when re-entered); "
                "compile_module with produce_cache from the top level instead");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            goto fail;
        }

        // BufferNotOwned: the Uint8Array bytes are pinned by the deserialized
        // request and stay valid through this v8_compile_module call; the
        // CachedData destructor (when source_obj falls out of scope) won't
        // free them.
        v8::ScriptCompiler::CachedData *cached_in = nullptr;
        if (cached_v->IsArrayBufferView()) {
            auto view = cached_v.As<v8::ArrayBufferView>();
            int len = static_cast<int>(view->ByteLength());
            if (len > 0) {
                auto store = view->Buffer()->GetBackingStore();
                auto bytes = static_cast<const uint8_t*>(store->Data()) + view->ByteOffset();
                cached_in = new v8::ScriptCompiler::CachedData(
                    bytes, len, v8::ScriptCompiler::CachedData::BufferNotOwned);
            }
        }

        // is_module must be true on the ScriptOrigin for V8 to accept
        // import/export syntax.
        v8::ScriptOrigin origin(filename,
                                /*resource_line_offset=*/0,
                                /*resource_column_offset=*/0,
                                /*resource_is_shared_cross_origin=*/false,
                                /*script_id=*/-1,
                                /*source_map_url=*/v8::Local<v8::Value>(),
                                /*resource_is_opaque=*/false,
                                /*is_wasm=*/false,
                                /*is_module=*/true);
        v8::ScriptCompiler::Source source_obj(source, origin, cached_in);
        auto options = cached_in ? v8::ScriptCompiler::kConsumeCodeCache
                                 : v8::ScriptCompiler::kNoCompileOptions;
        v8::Local<v8::Module> module;
        cause = PARSE_ERROR;
        if (!v8::ScriptCompiler::CompileModule(st.isolate, &source_obj, options)
            .ToLocal(&module)) goto fail;
        cause = INTERNAL_ERROR;

        bool rejected = (cached_in && source_obj.GetCachedData()->rejected);
        v8::Local<v8::Value> cache_value = v8::Null(st.isolate);
        if (produce_cache && (!cached_in || rejected)) {
            std::unique_ptr<v8::ScriptCompiler::CachedData> blob(
                v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
            if (blob && blob->length > 0) {
                auto backing = v8::ArrayBuffer::NewBackingStore(st.isolate, blob->length);
                memcpy(backing->Data(), blob->data, blob->length);
                cache_value = v8::ArrayBuffer::New(st.isolate, std::move(backing));
            }
        }

        // Ids are monotonic and serialized as Int32 on the wire. Refuse to
        // wrap rather than invoke signed-overflow UB and risk aliasing a
        // still-live handle id (unreachable in practice — each live module
        // pins a Global handle, so the isolate OOMs long before 2^31).
        if (cur(st).next_module_id == INT32_MAX) {
            cause = INTERNAL_ERROR;
            auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                "module id space exhausted for this Context");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            goto fail;
        }
        int32_t id = ++cur(st).next_module_id;
        auto entry = std::make_unique<ModuleEntry>();
        entry->handle.Reset(st.isolate, module);
        v8::String::Utf8Value fname(st.isolate, filename);
        if (*fname) entry->filename.assign(*fname, fname.length());

        {
            v8::Context::Scope context_scope(st.safe_context);
            result = v8::Array::New(st.isolate, 3);
        }
        // Populate via the goto-fail idiom, not .Check(): compile_module runs
        // under the watchdog (tag 'O' -> v8_timedwait), so a timeout can leave
        // the isolate terminating here, making Set() return Nothing — .Check()
        // would abort the process. The fail path replies a proper
        // TERMINATED_ERROR instead. (mirrors v8_compile)
        if (!result->Set(st.context, 0, v8::Int32::New(st.isolate, id)).FromMaybe(false)) goto fail;
        if (!result->Set(st.context, 1, cache_value).FromMaybe(false)) goto fail;
        if (!result->Set(st.context, 2, v8::Boolean::New(st.isolate, rejected)).FromMaybe(false)) goto fail;

        // Register the module only after the reply array is fully built. If a
        // Set above bailed (e.g. watchdog termination), the Ruby side gets an
        // error and never learns the id, so it could never erase the entry —
        // inserting earlier would orphan an undisposable handle until teardown.
        if (module_trace_on())
            fprintf(stderr, "[mr.register] url=%s id=%d (compile_module)\n",
                    *fname ? *fname : "?", id), fflush(stderr);
        cur(st).modules[id] = std::move(entry);
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    v8::Local<v8::Value> result_v = result.IsEmpty()
        ? static_cast<v8::Local<v8::Value>>(v8::Undefined(st.isolate))
        : static_cast<v8::Local<v8::Value>>(result);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result_v, err)) {
        assert(try_catch.HasCaught());
        goto fail;
    }
}

// V8 invokes this for each static import while InstantiateModule walks
// the import graph. It has no embedder slot, so State is recovered via
// isolate->GetData(0). We round-trip to Ruby with marker 'm', expect an
// int32 handle id back, and look it up in cur(st).modules.
//
// The Ruby resolver block can re-enter the v8 thread via other dispatch
// tags (e.g. compile_module the requested module on demand) — that flows
// through v8_dispatch inside the wait loop, like v8_api_callback does.
static v8::MaybeLocal<v8::Module> resolve_module_callback(
    v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> /*import_assertions*/,
    v8::Local<v8::Module> referrer)
{
    v8::Isolate *isolate = context->GetIsolate();
    State *pst = static_cast<State*>(isolate->GetData(0));
    State& st = *pst;
    // Suspended in a host->Ruby roundtrip for the whole resolve exchange.
    CallbackGuard _guard(st);

    // InstantiateModule walks the entire import graph in one call; without
    // an explicit scope, every Local allocated per import (request, dispatch
    // buffers, transitive compile_module Locals) would pile into whatever
    // outer scope the embedder installed. EscapableHandleScope so the
    // returned Local<Module> survives the scope's destruction.
    v8::EscapableHandleScope handle_scope(isolate);

    v8::Local<v8::Array> request;
    {
        v8::Context::Scope context_scope(st.safe_context);
        request = v8::Array::New(st.isolate, 2);
    }
    // Use the callback's |context| (matches what V8 walked the graph in)
    // rather than st.context. In mini_racer's single-context-per-isolate
    // model they're the same handle, but this is defensive in case that
    // ever changes.
    // .Check() would abort if a watchdog termination lands here; propagate the
    // pending exception instead (same convention as the serialization-failure
    // return below).
    if (request->Set(context, 0, specifier).IsNothing()) return v8::MaybeLocal<v8::Module>();
    // Referrer URL — the filename passed to compile_module's filename:
    // kwarg. Lets the Ruby resolver resolve relative specifiers
    // (`./foo`, `../bar`) against the importing module. Falls back to
    // an empty string if we can't materialize the v8::String (OOM).
    // Pass length explicitly so embedded NULs in the filename survive.
    v8::Local<v8::Value> referrer_name;
    v8::Local<v8::String> s;
    const std::string& ref_fn = module_filename(st, referrer);
    auto type = v8::NewStringType::kNormal;
    if (v8::String::NewFromUtf8(st.isolate, ref_fn.data(), type,
                                static_cast<int>(ref_fn.size())).ToLocal(&s)) {
        referrer_name = s;
    } else {
        referrer_name = v8::String::Empty(st.isolate);
    }
    if (request->Set(context, 1, referrer_name).IsNothing()) return v8::MaybeLocal<v8::Module>();
    {
        Serialized serialized(st, request);
        if (!serialized.data) return v8::MaybeLocal<v8::Module>();
        uint8_t marker = 'm';
        v8_reply(st.ruby_context, &marker, 1);
        v8_reply(st.ruby_context, serialized.data, serialized.size);
    }
    const uint8_t *p;
    size_t n;
    for (;;) {
        v8_roundtrip(st.ruby_context, &p, &n);
        if (*p == 'm') break;
        if (*p == 'e') {
            v8::Local<v8::String> message;
            auto type = v8::NewStringType::kNormal;
            if (!v8::String::NewFromOneByte(st.isolate, p+1, type, n-1).ToLocal(&message)) {
                message = v8::String::NewFromUtf8Literal(st.isolate, "Ruby exception");
            }
            auto exception = v8::Exception::Error(message);
            st.ruby_exception.Reset(st.isolate, exception);
            st.isolate->ThrowException(exception);
            return v8::MaybeLocal<v8::Module>();
        }
        v8_dispatch(st.ruby_context);
    }
    v8::ValueDeserializer des(st.isolate, p+1, n-1);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> id_v;
    if (!des.ReadValue(st.context).ToLocal(&id_v)) return v8::MaybeLocal<v8::Module>();
    int32_t id;
    if (!id_v->Int32Value(st.context).To(&id)) return v8::MaybeLocal<v8::Module>();
    auto it = cur(st).modules.find(id);
    if (it == cur(st).modules.end()) {
        auto msg = v8::String::NewFromUtf8Literal(st.isolate,
            "module resolver returned a handle unknown to this Context");
        st.isolate->ThrowException(v8::Exception::Error(msg));
        return v8::MaybeLocal<v8::Module>();
    }
    return handle_scope.Escape(v8::Local<v8::Module>::New(st.isolate,
                                                          it->second->handle));
}

// request: [handle_id:Int32]
// response: errback [undefined, err]
extern "C" void v8_instantiate_module(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Module> module;
        if (!module_from_request(st, des, &module, &cause)) goto fail;
        cause = RUNTIME_ERROR;
        v8::Maybe<bool> ok = module->InstantiateModule(st.context, resolve_module_callback);
        if (ok.IsNothing() || !ok.FromJust()) goto fail;
        result = v8::Undefined(st.isolate);
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (result.IsEmpty()) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail;
    }
}

// request: [handle_id:Int32]
// response: errback [evaluation_result, err]
//
// V8 wraps every module evaluation in a Promise (settles synchronously for
// non-TLA modules). We drain microtasks once, then unwrap. Pending after
// the drain means the module has top-level await still in flight — not
// supported in this round; the user gets a clear error.
extern "C" void v8_evaluate_module(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Module> module;
        if (!module_from_request(st, des, &module, &cause)) goto fail;
        // V8 requires status >= kInstantiated for Evaluate; calling on an
        // uninstantiated module hits a CHECK and aborts the process.
        if (module->GetStatus() < v8::Module::kInstantiated) {
            cause = RUNTIME_ERROR;
            auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                "module must be instantiated before it can be evaluated");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            goto fail;
        }
        cause = RUNTIME_ERROR;
        v8::Local<v8::Value> eval_result;
        if (!module->Evaluate(st.context).ToLocal(&eval_result)) goto fail;
        st.isolate->PerformMicrotaskCheckpoint();
        if (!eval_result->IsPromise()) {
            // older V8 / unusual configurations may return a plain value
            result = sanitize(st, eval_result);
        } else {
            auto promise = eval_result.As<v8::Promise>();
            if (promise->State() == v8::Promise::kFulfilled) {
                result = sanitize(st, promise->Result());
            } else if (promise->State() == v8::Promise::kRejected) {
                st.isolate->ThrowException(promise->Result());
                goto fail;
            } else {
                auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                    "module evaluation is still pending "
                    "(top-level await is not yet supported)");
                st.isolate->ThrowException(v8::Exception::Error(msg));
                goto fail;
            }
        }
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (result.IsEmpty()) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail;
    }
}

// ---- Context#load_module_graph: batched, mostly Ruby-free ESM graph load ----

static std::string edge_key(const std::string& referrer, const std::string& specifier)
{
    std::string k;
    k.reserve(referrer.size() + 1 + specifier.size());
    k.append(referrer);
    k.push_back('\0');
    k.append(specifier);
    return k;
}

// URL registry: one Module instance per URL for the realm's lifetime.
static v8::Local<v8::Module> registry_lookup(State& st, const std::string& url)
{
    auto it = cur(st).module_id_by_url.find(url);
    if (it == cur(st).module_id_by_url.end()) return v8::Local<v8::Module>();
    auto m = cur(st).modules.find(it->second);
    if (m == cur(st).modules.end()) return v8::Local<v8::Module>();
    return v8::Local<v8::Module>::New(st.isolate, m->second->handle);
}

// Register a freshly compiled module under |url| (filename=url so module_filename
// and import.meta.url resolve it). Caller must have confirmed a registry miss.
static void registry_register(State& st, const std::string& url, v8::Local<v8::Module> module)
{
    int32_t id = ++cur(st).next_module_id;
    auto entry = std::make_unique<ModuleEntry>();
    entry->handle.Reset(st.isolate, module);
    entry->filename = url;
    cur(st).modules[id] = std::move(entry);
    cur(st).module_id_by_url[url] = id;
    if (module_trace_on())
        fprintf(stderr, "[mr.register] url=%s id=%d (registry)\n", url.c_str(), id), fflush(stderr);
}

// Undo registry_register for |urls| — used to roll back a load that registered
// modules but then failed to instantiate/evaluate, so those URLs aren't left in
// the registry as half-loaded (uninstantiated) modules that future imports would
// reuse and reject. Only the modules a failed load itself compiled are passed in;
// reused modules from earlier successful loads are untouched.
static void registry_rollback(State& st, const std::vector<std::string>& urls)
{
    for (const std::string& url : urls) {
        auto it = cur(st).module_id_by_url.find(url);
        if (it == cur(st).module_id_by_url.end()) continue;
        cur(st).modules.erase(it->second);
        cur(st).module_id_by_url.erase(it);
    }
}

// InstantiateModule resolver for a graph/dynamic load. Every edge was resolved
// during the walk and its target is in the URL registry, so this is a pure map
// lookup — no Ruby round-trip per import. Returns empty (throwing) for edges the
// embedder left unresolved (resolve -> nil) or whose target failed to fetch
// (404); InstantiateModule then fails on that import (ESM-correct for a missing
// static dependency).
static v8::MaybeLocal<v8::Module> graph_resolve_callback(
    v8::Local<v8::Context> /*context*/,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> /*import_assertions*/,
    v8::Local<v8::Module> referrer)
{
    State& st = *static_cast<State*>(v8::Isolate::GetCurrent()->GetData(0));
    auto isolate = st.isolate;
    v8::String::Utf8Value spec(isolate, specifier);
    std::string spec_s(*spec ? *spec : "", *spec ? spec.length() : 0);
    if (cur(st).active_graph) {
        const std::string& ref_url = module_filename(st, referrer);
        auto e = cur(st).active_graph->edges.find(edge_key(ref_url, spec_s));
        if (e != cur(st).active_graph->edges.end() && !e->second.empty()) {
            auto m = registry_lookup(st, e->second);
            if (!m.IsEmpty()) return m;
        }
    }
    std::string msg = "could not resolve import \"";
    msg.append(spec_s).append("\"");
    v8::Local<v8::String> m;
    if (!v8::String::NewFromUtf8(isolate, msg.c_str()).ToLocal(&m))
        m = v8::String::NewFromUtf8Literal(isolate, "could not resolve import");
    isolate->ThrowException(v8::Exception::Error(m));
    return v8::MaybeLocal<v8::Module>();
}

// Send |request| to Ruby under |marker|, pump the rendezvous wait loop (the Ruby
// handler may re-enter via v8_dispatch), and deserialize the reply (returned
// under the same marker). Returns false with an exception pending if Ruby raised
// ('e') or (de)serialization failed.
static bool graph_roundtrip(State& st, char marker, v8::Local<v8::Value> request,
                            v8::Local<v8::Value>* reply_out)
{
    // Suspended in a host->Ruby roundtrip: the fetch/resolve block runs while
    // this v8_load_module_graph frame (and its cur(st).active_graph) sits on the C++
    // stack. Mark in_callback so reset_realm refuses and compile() refuses
    // CreateCodeCache — re-entering either from here would corrupt the realm or
    // V8's parser.
    CallbackGuard _guard(st);
    {
        Serialized serialized(st, request);
        if (!serialized.data) return false; // exception pending
        uint8_t m = static_cast<uint8_t>(marker);
        v8_reply(st.ruby_context, &m, 1);
        v8_reply(st.ruby_context, serialized.data, serialized.size);
    }
    const uint8_t *p;
    size_t n;
    for (;;) {
        v8_roundtrip(st.ruby_context, &p, &n);
        if (*p == static_cast<uint8_t>(marker)) break;
        if (*p == 'e') {
            v8::Local<v8::String> message;
            auto type = v8::NewStringType::kNormal;
            if (!v8::String::NewFromOneByte(st.isolate, p+1, type, n-1).ToLocal(&message))
                message = v8::String::NewFromUtf8Literal(st.isolate, "Ruby exception");
            auto exception = v8::Exception::Error(message);
            st.ruby_exception.Reset(st.isolate, exception);
            st.isolate->ThrowException(exception);
            return false;
        }
        v8_dispatch(st.ruby_context);
    }
    v8::ValueDeserializer des(st.isolate, p+1, n-1);
    des.ReadHeader(st.context).Check();
    return des.ReadValue(st.context).ToLocal(reply_out);
}

// Make a JS string from a std::string, preserving length (embedded NULs/UTF-8).
static bool graph_str(State& st, const std::string& s, v8::Local<v8::String>* out)
{
    return v8::String::NewFromUtf8(st.isolate, s.data(), v8::NewStringType::kNormal,
                                   static_cast<int>(s.size())).ToLocal(out);
}

// Compile one module for the walk; sets *rejected if a supplied code cache was
// rejected by V8. Mirrors v8_compile_module's compile path (is_module origin,
// BufferNotOwned cached_data) but does not produce caches.
static v8::MaybeLocal<v8::Module> graph_compile(State& st, v8::Local<v8::String> filename,
                                                v8::Local<v8::String> source,
                                                v8::Local<v8::Value> cached_v, bool* rejected)
{
    *rejected = false;
    v8::ScriptCompiler::CachedData *cached_in = nullptr;
    if (cached_v->IsArrayBufferView()) {
        auto view = cached_v.As<v8::ArrayBufferView>();
        int len = static_cast<int>(view->ByteLength());
        if (len > 0) {
            auto store = view->Buffer()->GetBackingStore();
            auto bytes = static_cast<const uint8_t*>(store->Data()) + view->ByteOffset();
            cached_in = new v8::ScriptCompiler::CachedData(
                bytes, len, v8::ScriptCompiler::CachedData::BufferNotOwned);
        }
    }
    v8::ScriptOrigin origin(filename, 0, 0, false, -1, v8::Local<v8::Value>(),
                            false, false, /*is_module=*/true);
    v8::ScriptCompiler::Source source_obj(source, origin, cached_in);
    auto options = cached_in ? v8::ScriptCompiler::kConsumeCodeCache
                             : v8::ScriptCompiler::kNoCompileOptions;
    v8::Local<v8::Module> module;
    if (!v8::ScriptCompiler::CompileModule(st.isolate, &source_obj, options).ToLocal(&module))
        return v8::MaybeLocal<v8::Module>();
    if (cached_in) *rejected = source_obj.GetCachedData()->rejected;
    return module;
}

// Loads the modules reachable from |entry_url| that aren't already in the URL
// registry, using the Context's persisted resolve/fetch_batch callbacks. Walks
// level by level: fetch a batch ('f'), compile + register each module with its
// cached_data, collect its imports, batch-resolve them ('r'), recurse into the
// not-yet-registered targets. Records every resolved edge into |edges| and
// appends newly compiled URLs to |new_urls| (with per-URL cache_rejected).
// Already-registered URLs are reused — never re-fetched or re-compiled, so
// dynamic import() of a URL the entry graph already pulled in gets the same
// Module instance. Returns false with an exception pending on error.
static bool walk_module_graph(State& st, const std::string& entry_url,
                              std::unordered_map<std::string, std::string>& edges,
                              std::vector<std::string>& new_urls,
                              std::unordered_map<std::string, bool>& rejected_by_url)
{
    std::unordered_set<std::string> seen;
    std::vector<std::string> to_fetch;
    if (registry_lookup(st, entry_url).IsEmpty()) {
        to_fetch.push_back(entry_url);
        seen.insert(entry_url);
    }
    while (!to_fetch.empty()) {
        // ---- FETCH batch ----
        v8::Local<v8::Array> urls_arr;
        { v8::Context::Scope cs(st.safe_context); urls_arr = v8::Array::New(st.isolate, (int)to_fetch.size()); }
        for (size_t i = 0; i < to_fetch.size(); i++) {
            v8::Local<v8::String> u;
            if (!graph_str(st, to_fetch[i], &u)) return false;
            // A watchdog/OOM termination makes Set() return Nothing; .Check()
            // would abort the process. Propagate as this function's documented
            // "false with an exception pending" so v8_load_module_graph's fail
            // epilogue cancels the termination and replies TERMINATED.
            if (urls_arr->Set(st.context, (uint32_t)i, u).IsNothing()) return false;
        }
        v8::Local<v8::Value> fetched_v;
        if (!graph_roundtrip(st, 'f', urls_arr, &fetched_v)) return false;
        if (!fetched_v->IsArray()) return false;
        auto fetched = fetched_v.As<v8::Array>();

        // ---- compile + register this level, collect edges ----
        std::vector<std::pair<std::string, std::string>> level_edges; // (specifier, referrer_url)
        std::unordered_set<std::string> edge_seen;                     // dedup (ref,spec)
        for (size_t i = 0; i < to_fetch.size(); i++) {
            const std::string url = to_fetch[i];
            v8::Local<v8::Value> entry;
            if (!fetched->Get(st.context, (uint32_t)i).ToLocal(&entry)) return false;
            // nil / non-pair => fetch failed (404). Leave it uncompiled: any
            // static import of it then fails at instantiate (ESM-correct for a
            // missing dependency). Not added to new_urls (it was not loaded).
            if (!entry->IsArray()) continue;
            auto pair = entry.As<v8::Array>();
            v8::Local<v8::Value> source_v, cached_v;
            if (!pair->Get(st.context, 0).ToLocal(&source_v)) return false;
            if (!pair->Get(st.context, 1).ToLocal(&cached_v)) return false;
            v8::Local<v8::String> source, fname;
            if (!source_v->ToString(st.context).ToLocal(&source)) return false;
            if (!graph_str(st, url, &fname)) return false;
            bool rej = false;
            v8::Local<v8::Module> module;
            if (!graph_compile(st, fname, source, cached_v, &rej).ToLocal(&module)) return false;
            registry_register(st, url, module);
            rejected_by_url[url] = rej;
            new_urls.push_back(url);
            // Collect imports for the resolve batch (deduped: `import a from "x";
            // import b from "x"` is one edge).
            auto requests = module->GetModuleRequests();
            for (int r = 0; r < requests->Length(); r++) {
                auto mr = requests->Get(st.context, r).As<v8::ModuleRequest>();
                v8::String::Utf8Value spec(st.isolate, mr->GetSpecifier());
                if (!*spec) continue;
                std::string spec_s(*spec, spec.length());
                if (edge_seen.insert(edge_key(url, spec_s)).second)
                    level_edges.emplace_back(spec_s, url);
            }
        }

        // ---- RESOLVE batch ----
        to_fetch.clear();
        if (level_edges.empty()) continue;
        v8::Local<v8::Array> edges_arr;
        { v8::Context::Scope cs(st.safe_context); edges_arr = v8::Array::New(st.isolate, (int)level_edges.size()); }
        for (size_t i = 0; i < level_edges.size(); i++) {
            v8::Local<v8::Array> pr;
            { v8::Context::Scope cs(st.safe_context); pr = v8::Array::New(st.isolate, 2); }
            v8::Local<v8::String> spec, ref;
            if (!graph_str(st, level_edges[i].first, &spec)) return false;
            if (!graph_str(st, level_edges[i].second, &ref)) return false;
            if (pr->Set(st.context, 0, spec).IsNothing()) return false;
            if (pr->Set(st.context, 1, ref).IsNothing()) return false;
            if (edges_arr->Set(st.context, (uint32_t)i, pr).IsNothing()) return false;
        }
        v8::Local<v8::Value> resolved_v;
        if (!graph_roundtrip(st, 'r', edges_arr, &resolved_v)) return false;
        if (!resolved_v->IsArray()) return false;
        auto resolved = resolved_v.As<v8::Array>();
        for (size_t i = 0; i < level_edges.size(); i++) {
            v8::Local<v8::Value> u;
            if (!resolved->Get(st.context, (uint32_t)i).ToLocal(&u)) return false;
            std::string turl;
            if (u->IsString()) {
                v8::String::Utf8Value uu(st.isolate, u);
                if (*uu) turl.assign(*uu, uu.length());
            }
            edges[edge_key(level_edges[i].second, level_edges[i].first)] = turl;
            if (!turl.empty() && registry_lookup(st, turl).IsEmpty() && seen.find(turl) == seen.end()) {
                seen.insert(turl);
                to_fetch.push_back(turl);
            }
        }
    }
    return true;
}

// Instantiate (native resolver) + Evaluate |entry_module| under |graph|'s edges,
// draining microtasks and rejecting on top-level await. Writes the evaluation
// value to *out. Returns false with an exception pending on failure. active_graph
// is save/restored (not blindly nulled) so a nested dynamic import() fired during
// this Evaluate doesn't clobber an enclosing load's graph.
static bool instantiate_and_evaluate(State& st, GraphLoad& graph,
                                     v8::Local<v8::Module> entry_module,
                                     v8::Local<v8::Value>* out)
{
    GraphLoad *prev = cur(st).active_graph;
    cur(st).active_graph = &graph;
    v8::Maybe<bool> ok = entry_module->InstantiateModule(st.context, graph_resolve_callback);
    if (ok.IsNothing() || !ok.FromJust()) { cur(st).active_graph = prev; return false; }
    v8::Local<v8::Value> eval_result;
    if (!entry_module->Evaluate(st.context).ToLocal(&eval_result)) { cur(st).active_graph = prev; return false; }
    st.isolate->PerformMicrotaskCheckpoint();
    cur(st).active_graph = prev;
    if (!eval_result->IsPromise()) { *out = sanitize(st, eval_result); return true; }
    auto promise = eval_result.As<v8::Promise>();
    if (promise->State() == v8::Promise::kFulfilled) { *out = sanitize(st, promise->Result()); return true; }
    if (promise->State() == v8::Promise::kRejected) { st.isolate->ThrowException(promise->Result()); return false; }
    auto msg = v8::String::NewFromUtf8Literal(st.isolate,
        "module evaluation is still pending (top-level await is not yet supported)");
    st.isolate->ThrowException(v8::Exception::Error(msg));
    return false;
}

// request: [entry_url:String]
// response: errback [[value, [[url, cache_rejected:Bool], ...]], err]
//
// Walks the static import graph on the V8 thread (see walk_module_graph), then
// instantiates with a native resolver and evaluates. Collapses the per-module
// compile_module/instantiate round-trips (~2*N) down to ~2 per graph level, and
// registers every module in the URL registry so later dynamic import() reuses
// the same instances. `modules` lists only modules newly compiled by this call.
extern "C" void v8_load_module_graph(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    // Route dynamic import() through the registry for the rest of this load
    // (the entry's own top-level import() must reuse it) and, on success, for
    // the Context's life. Reverted below if this load fails, so a failed first
    // load doesn't permanently disable the legacy dynamic_import_resolver.
    bool prev_uses_graph_loader = cur(st).uses_graph_loader;
    cur(st).uses_graph_loader = true;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    GraphLoad graph;
    std::vector<std::string> new_urls;
    std::unordered_map<std::string, bool> rejected_by_url;
    {
        v8::Local<v8::Value> req_v;
        if (!des.ReadValue(st.context).ToLocal(&req_v)) goto fail;
        v8::Local<v8::Object> req;
        if (!req_v->ToObject(st.context).ToLocal(&req)) goto fail;
        v8::Local<v8::Value> entry_v;
        if (!req->Get(st.context, 0).ToLocal(&entry_v)) goto fail;
        v8::String::Utf8Value entry_u(st.isolate, entry_v);
        if (!*entry_u) goto fail;
        std::string entry_url(*entry_u, entry_u.length());

        cause = RUNTIME_ERROR;
        if (!walk_module_graph(st, entry_url, graph.edges, new_urls, rejected_by_url)) goto fail;

        v8::Local<v8::Module> entry_module = registry_lookup(st, entry_url);
        if (entry_module.IsEmpty()) {
            auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                "load_module_graph: entry module could not be fetched");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            goto fail;
        }

        v8::Local<v8::Value> value;
        if (!instantiate_and_evaluate(st, graph, entry_module, &value)) goto fail;

        // ---- build [value, [[url, rejected], ...]] for newly compiled modules ----
        cause = INTERNAL_ERROR;
        v8::Local<v8::Array> mods;
        { v8::Context::Scope cs(st.safe_context); mods = v8::Array::New(st.isolate, (int)new_urls.size()); }
        for (size_t i = 0; i < new_urls.size(); i++) {
            v8::Local<v8::Array> row;
            { v8::Context::Scope cs(st.safe_context); row = v8::Array::New(st.isolate, 2); }
            v8::Local<v8::String> u;
            if (!graph_str(st, new_urls[i], &u)) goto fail;
            // Building the reply runs under the watchdog ('G' -> v8_timedwait);
            // a termination here makes Set() return Nothing. goto fail (not
            // .Check()/abort) so the epilogue replies TERMINATED.
            if (row->Set(st.context, 0, u).IsNothing()) goto fail;
            if (row->Set(st.context, 1, v8::Boolean::New(st.isolate, rejected_by_url[new_urls[i]])).IsNothing()) goto fail;
            if (mods->Set(st.context, (uint32_t)i, row).IsNothing()) goto fail;
        }
        v8::Local<v8::Array> out;
        { v8::Context::Scope cs(st.safe_context); out = v8::Array::New(st.isolate, 2); }
        if (out->Set(st.context, 0, value).IsNothing()) goto fail;
        if (out->Set(st.context, 1, mods).IsNothing()) goto fail;
        result = out;
    }
    cause = NO_ERROR;
fail:
    cur(st).active_graph = nullptr;
    // On failure (every goto-fail sets a nonzero cause; success and the
    // reply-retry path leave it NO_ERROR), undo this load's registrations so it
    // leaves no half-loaded modules behind, and revert the loader latch so a
    // failed load doesn't disable the legacy resolver.
    if (cause != NO_ERROR) {
        registry_rollback(st, new_urls);
        cur(st).uses_graph_loader = prev_uses_graph_loader;
    }
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    v8::Local<v8::Value> result_v = result.IsEmpty()
        ? static_cast<v8::Local<v8::Value>>(v8::Undefined(st.isolate))
        : result;
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result_v, err)) {
        assert(try_catch.HasCaught());
        goto fail;
    }
}

// request: [handle_id:Int32]
// response: errback [namespace_value, err]
//
// GetModuleNamespace requires the module to be at least instantiated
// (V8 will fatal otherwise). Plain-data exports come back as Hash
// entries via the regular sanitize path; function exports are filtered
// out by the safe-context wrapper, same as other Object returns.
extern "C" void v8_module_namespace(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Module> module;
        if (!module_from_request(st, des, &module, &cause)) goto fail;
        // Only a fully evaluated, non-async module has a safe-to-read namespace.
        // Reading bindings still in the temporal dead zone (not yet evaluated,
        // or a top-level-await module whose promise never settled) makes the
        // serializer hit a throwing accessor on every property, which V8 turns
        // into an unrecoverable FatalProcessOutOfMemory (process abort), not a
        // catchable exception. Require kEvaluated AND !IsGraphAsync; surface an
        // errored module's own exception; reject every other state.
        auto status = module->GetStatus();
        if (status == v8::Module::kErrored) {
            cause = RUNTIME_ERROR;
            st.isolate->ThrowException(module->GetException());
            goto fail;
        }
        if (status != v8::Module::kEvaluated || module->IsGraphAsync()) {
            cause = RUNTIME_ERROR;
            auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                "module must be evaluated (and not use top-level await) before "
                "its namespace can be read");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            goto fail;
        }
        cause = RUNTIME_ERROR;
        result = sanitize(st, module->GetModuleNamespace());
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (result.IsEmpty()) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail;
    }
}

// response: errback [status_name:String, err]
// status_name is one of the v8::Module::Status enum names in lowercase
// ("uninstantiated", "instantiating", "instantiated", "evaluating",
// "evaluated", "errored"). Ruby side converts to a symbol.
extern "C" void v8_module_status(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Module> module;
        if (!module_from_request(st, des, &module, &cause)) goto fail;
        const char *name;
        switch (module->GetStatus()) {
        case v8::Module::kUninstantiated: name = "uninstantiated"; break;
        case v8::Module::kInstantiating:  name = "instantiating";  break;
        case v8::Module::kInstantiated:   name = "instantiated";   break;
        case v8::Module::kEvaluating:     name = "evaluating";     break;
        case v8::Module::kEvaluated:      name = "evaluated";      break;
        case v8::Module::kErrored:        name = "errored";        break;
        default:                          name = "unknown";        break;
        }
        v8::Local<v8::String> s;
        if (!v8::String::NewFromUtf8(st.isolate, name).ToLocal(&s)) goto fail;
        result = s;
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (result.IsEmpty()) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail;
    }
}

// Unknown ids are silently ignored — Ruby-side Module#dispose is idempotent.
extern "C" void v8_dispose_module(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> id_v;
    if (des.ReadValue(st.context).ToLocal(&id_v)) {
        int32_t id;
        if (id_v->Int32Value(st.context).To(&id))
            cur(st).modules.erase(id);
    }
    reply_retry(st, v8::String::Empty(st.isolate));
}

// request: [filename, source, cached_data|null, produce_cache:Bool]
// response: errback [[handle_id:Int32, cached_data:ArrayBuffer|null, rejected:Bool], err]
//
// CreateCodeCache walks live isolate state in a way that corrupts the parser
// when called from inside a v8_api_callback frame (re-entrant compile from
// host fn). Callers must opt in via produce_cache and only do so from the
// top level; we raise from re-entrant context rather than silently skipping
// so misuse is caught immediately.
extern "C" void v8_compile(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Array> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request;
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> filename;
        if (!request->Get(st.context, 0).ToLocal(&filename)) goto fail;
        v8::Local<v8::Value> source_v;
        if (!request->Get(st.context, 1).ToLocal(&source_v)) goto fail;
        v8::Local<v8::Value> cached_v;
        if (!request->Get(st.context, 2).ToLocal(&cached_v)) goto fail;
        v8::Local<v8::Value> produce_v;
        if (!request->Get(st.context, 3).ToLocal(&produce_v)) goto fail;
        bool produce_cache = produce_v->BooleanValue(st.isolate);
        v8::Local<v8::String> source;
        if (!source_v->ToString(st.context).ToLocal(&source)) goto fail;

        if (produce_cache && st.in_callback > 0) {
            cause = RUNTIME_ERROR;
            auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                "produce_cache: true is unsafe inside a host-function callback "
                "(V8 CreateCodeCache corrupts parser state when re-entered); "
                "compile with produce_cache from the top level instead");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            goto fail;
        }

        // ser_uint8array on the Ruby side wraps the bytes in an ArrayBuffer +
        // Uint8Array view. The view's backing bytes are valid for the whole
        // v8_compile call, so BufferNotOwned avoids a copy — the CachedData
        // destructor (run when source_obj goes out of scope) leaves them alone.
        v8::ScriptCompiler::CachedData *cached_in = nullptr;
        if (cached_v->IsArrayBufferView()) {
            auto view = cached_v.As<v8::ArrayBufferView>();
            int len = static_cast<int>(view->ByteLength());
            if (len > 0) {
                auto store = view->Buffer()->GetBackingStore();
                auto bytes = static_cast<const uint8_t*>(store->Data()) + view->ByteOffset();
                cached_in = new v8::ScriptCompiler::CachedData(
                    bytes, len, v8::ScriptCompiler::CachedData::BufferNotOwned);
            }
        }

        v8::ScriptOrigin origin(filename);
        v8::ScriptCompiler::Source source_obj(source, origin, cached_in);
        auto options = cached_in ? v8::ScriptCompiler::kConsumeCodeCache
                                 : v8::ScriptCompiler::kNoCompileOptions;
        v8::Local<v8::Script> script;
        cause = PARSE_ERROR;
        if (!v8::ScriptCompiler::Compile(st.context, &source_obj, options)
            .ToLocal(&script)) goto fail;
        cause = INTERNAL_ERROR;

        bool rejected = (cached_in && source_obj.GetCachedData()->rejected);
        v8::Local<v8::Value> cache_value = v8::Null(st.isolate);
        if (produce_cache && (!cached_in || rejected)) {
            std::unique_ptr<v8::ScriptCompiler::CachedData> blob(
                v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript()));
            if (blob && blob->length > 0) {
                auto backing = v8::ArrayBuffer::NewBackingStore(st.isolate, blob->length);
                memcpy(backing->Data(), blob->data, blob->length);
                cache_value = v8::ArrayBuffer::New(st.isolate, std::move(backing));
            }
        }

        // Ids are monotonic and serialized as Int32 on the wire. Refuse to
        // wrap at INT32_MAX rather than invoke signed-overflow UB / risk
        // aliasing a still-live id (unreachable in practice — each undisposed
        // script pins a handle, so the isolate OOMs long before 2^31).
        if (cur(st).next_script_id == INT32_MAX) {
            cause = INTERNAL_ERROR;
            auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                "script id space exhausted for this Context");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            goto fail;
        }
        int32_t id = ++cur(st).next_script_id;

        {
            v8::Context::Scope context_scope(st.safe_context);
            result = v8::Array::New(st.isolate, 3);
        }
        // Populate via the goto-fail idiom, not .Check(): v8_compile runs under
        // the watchdog ('K' -> v8_timedwait), so a timeout can leave the isolate
        // terminating here, making Set() return Nothing — .Check() would abort
        // the process. The fail path replies a proper TERMINATED_ERROR instead.
        if (!result->Set(st.context, 0, v8::Int32::New(st.isolate, id)).FromMaybe(false)) goto fail;
        if (!result->Set(st.context, 1, cache_value).FromMaybe(false)) goto fail;
        if (!result->Set(st.context, 2, v8::Boolean::New(st.isolate, rejected)).FromMaybe(false)) goto fail;

        // Register the handle only after the reply array is fully built. If a
        // Set above bailed (e.g. watchdog termination), the Ruby side gets an
        // error and never learns the id, so it could never erase the entry —
        // inserting earlier would orphan an undisposable handle until teardown.
        cur(st).scripts[id].Reset(st.isolate, script);
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    v8::Local<v8::Value> result_v = result.IsEmpty()
        ? static_cast<v8::Local<v8::Value>>(v8::Undefined(st.isolate))
        : static_cast<v8::Local<v8::Value>>(result);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result_v, err)) {
        assert(try_catch.HasCaught());
        goto fail;
    }
}

extern "C" void v8_run(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> id_v;
        if (!des.ReadValue(st.context).ToLocal(&id_v)) goto fail;
        int32_t id;
        if (!id_v->Int32Value(st.context).To(&id)) goto fail;
        auto it = cur(st).scripts.find(id);
        if (it == cur(st).scripts.end()) {
            cause = RUNTIME_ERROR;
            auto msg = v8::String::NewFromUtf8Literal(st.isolate, "no such script handle");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            goto fail;
        }
        auto script = v8::Local<v8::Script>::New(st.isolate, it->second);
        v8::Local<v8::Value> result_v;
        cause = RUNTIME_ERROR;
        if (!script->Run(st.context).ToLocal(&result_v)) goto fail;
        result = sanitize(st, result_v);
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail;
    }
}

// Unknown ids are silently ignored — Ruby-side Script#dispose is idempotent.
extern "C" void v8_dispose_script(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> id_v;
    if (des.ReadValue(st.context).ToLocal(&id_v)) {
        int32_t id;
        if (id_v->Int32Value(st.context).To(&id))
            cur(st).scripts.erase(id);
    }
    reply_retry(st, v8::String::Empty(st.isolate));
}

extern "C" void v8_heap_stats(State *pst)
{
    State& st = *pst;
    v8::HandleScope handle_scope(st.isolate);
    v8::HeapStatistics s;
    st.isolate->GetHeapStatistics(&s);
    v8::Local<v8::Object> response = v8::Object::New(st.isolate);
    // Set() must not .Check(): heap_stats is dispatchable nested from inside a
    // host callback (ctx.heap_stats from a host function's Ruby body), so the
    // outer op's watchdog/OOM TerminateExecution can be pending here, making
    // Set() return Nothing — .Check() would abort the process. On termination
    // stop populating and reply what we have; reply_retry cancels the
    // termination to deliver the (possibly partial) stats and re-arms it so the
    // outer op still terminates.
#define PROP(name)                                                      \
    do {                                                                \
        auto key = v8::String::NewFromUtf8Literal(st.isolate, #name);   \
        auto val = v8::Number::New(st.isolate, s.name());               \
        if (response->Set(st.context, key, val).IsNothing()) goto done; \
    } while (0)
    PROP(total_heap_size);
    PROP(total_heap_size);
    PROP(total_heap_size_executable);
    PROP(total_physical_size);
    PROP(total_available_size);
    PROP(total_global_handles_size);
    PROP(used_global_handles_size);
    PROP(used_heap_size);
    PROP(heap_size_limit);
    PROP(malloced_memory);
    PROP(external_memory);
    PROP(peak_malloced_memory);
    PROP(number_of_native_contexts);
    PROP(number_of_detached_contexts);
#undef PROP
done:
    reply_retry(st, response);
}

struct OutputStream : public v8::OutputStream
{
    std::vector<uint8_t> buf;

    void EndOfStream() final {}
    int GetChunkSize() final { return 65536; }

    WriteResult WriteAsciiChunk(char* data, int size)
    {
        const uint8_t *p = reinterpret_cast<uint8_t*>(data);
        buf.insert(buf.end(), p, p+size);
        return WriteResult::kContinue;
    }
};

extern "C" void v8_heap_snapshot(State *pst)
{
    State& st = *pst;
    v8::HandleScope handle_scope(st.isolate);
    auto snapshot = st.isolate->GetHeapProfiler()->TakeHeapSnapshot();
    OutputStream os;
    snapshot->Serialize(&os, v8::HeapSnapshot::kJSON);
    v8_reply(st.ruby_context, os.buf.data(), os.buf.size()); // not serialized because big
}

extern "C" void v8_perform_microtask_checkpoint(State *pst)
{
    // Leave any termination active so the enclosing v8_call/v8_eval frame
    // surfaces OOM (set by v8_gc_callback) or watchdog termination to Ruby.
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::MicrotasksScope::PerformCheckpoint(st.isolate);
    notify_unhandled_rejections(st);
    reply_retry(st, v8::Undefined(st.isolate));
}

extern "C" void v8_pump_message_loop(State *pst)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    bool ran_task = v8::platform::PumpMessageLoop(platform, st.isolate);
    if (st.isolate->IsExecutionTerminating()) goto fail;
    if (try_catch.HasCaught()) goto fail;
    if (ran_task) {
        v8::MicrotasksScope::PerformCheckpoint(st.isolate);
        notify_unhandled_rejections(st);
    }
    if (st.isolate->IsExecutionTerminating()) goto fail;
    if (platform->IdleTasksEnabled(st.isolate)) {
        double idle_time_in_seconds = 1.0 / 50;
        v8::platform::RunIdleTasks(platform, st.isolate, idle_time_in_seconds);
        if (st.isolate->IsExecutionTerminating()) goto fail;
        if (try_catch.HasCaught()) goto fail;
    }
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        st.err_reason = NO_ERROR;
    }
    auto result = v8::Boolean::New(st.isolate, ran_task);
    reply_retry(st, result);
}

int snapshot(bool is_warmup, bool verbose_exceptions,
             const v8::String::Utf8Value& code,
             v8::StartupData blob, v8::StartupData *result,
             char (*errbuf)[512])
{
    // SnapshotCreator takes ownership of isolate
    v8::Isolate *isolate = v8::Isolate::Allocate();
    v8::StartupData *existing_blob = is_warmup ? &blob : nullptr;
    v8::SnapshotCreator snapshot_creator(isolate, nullptr, existing_blob);
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(verbose_exceptions);
    auto filename = is_warmup
        ? v8::String::NewFromUtf8Literal(isolate, "<warmup>")
        : v8::String::NewFromUtf8Literal(isolate, "<snapshot>");
    auto mode = is_warmup
        ? v8::SnapshotCreator::FunctionCodeHandling::kKeep
        : v8::SnapshotCreator::FunctionCodeHandling::kClear;
    int cause = INTERNAL_ERROR;
    {
        auto context = v8::Context::New(isolate);
        v8::Context::Scope context_scope(context);
        v8::Local<v8::String> source;
        auto type = v8::NewStringType::kNormal;
        if (!v8::String::NewFromUtf8(isolate, *code, type, code.length()).ToLocal(&source)) {
            v8::String::Utf8Value s(isolate, try_catch.Exception());
            if (*s) snprintf(*errbuf, sizeof(*errbuf), "%c%s", cause, *s);
            goto fail;
        }
        v8::ScriptOrigin origin(filename);
        v8::Local<v8::Script> script;
        cause = PARSE_ERROR;
        if (!v8::Script::Compile(context, source, &origin).ToLocal(&script)) {
            goto err;
        }
        cause = RUNTIME_ERROR;
        if (script->Run(context).IsEmpty()) {
        err:
            auto m = try_catch.Message();
            v8::String::Utf8Value s(isolate, m->Get());
            v8::String::Utf8Value name(isolate, m->GetScriptResourceName());
            auto line = m->GetLineNumber(context).FromMaybe(0);
            auto column = m->GetStartColumn(context).FromMaybe(0);
            snprintf(*errbuf, sizeof(*errbuf), "%c%s\n%s:%d:%d",
                     cause, *s, *name, line, column);
            goto fail;
        }
        cause = INTERNAL_ERROR;
        if (!is_warmup) snapshot_creator.SetDefaultContext(context);
    }
    if (is_warmup) {
        isolate->ContextDisposedNotification(false);
        auto context = v8::Context::New(isolate);
        snapshot_creator.SetDefaultContext(context);
    }
    *result = snapshot_creator.CreateBlob(mode);
    cause = NO_ERROR;
fail:
    return cause;
}

// response is errback [result, err] array
// note: currently needs --stress_snapshot in V8 debug builds
// to work around a buggy check in the snapshot deserializer
extern "C" void v8_snapshot(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    v8::StartupData blob{nullptr, 0};
    int cause = INTERNAL_ERROR;
    char errbuf[512] = {0};
    {
        v8::Local<v8::Value> code_v;
        if (!des.ReadValue(st.context).ToLocal(&code_v)) goto fail;
        v8::String::Utf8Value code(st.isolate, code_v);
        if (!*code) goto fail;
        v8::StartupData init{nullptr, 0};
        cause = snapshot(/*is_warmup*/false, st.verbose_exceptions, code, init, &blob, &errbuf);
        if (cause) goto fail;
    }
    if (blob.data) {
        auto data = reinterpret_cast<const uint8_t*>(blob.data);
        auto type = v8::NewStringType::kNormal;
        bool ok = v8::String::NewFromOneByte(st.isolate, data, type,
                                             blob.raw_size).ToLocal(&result);
        delete[] blob.data;
        blob = v8::StartupData{nullptr, 0};
        if (!ok) goto fail;
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
        // A watchdog/OOM termination preempting the reply supersedes any earlier
        // snapshot-failure message; drop it so we report the TERMINATED/MEMORY
        // error built from |cause| rather than the stale errbuf text.
        *errbuf = '\0';
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    v8::Local<v8::Value> err;
    if (*errbuf) {
        if (!v8::String::NewFromUtf8(st.isolate, errbuf).ToLocal(&err)) {
            err = v8::String::NewFromUtf8Literal(st.isolate, "unexpected error");
        }
    } else {
        err = to_error(st, &try_catch, cause);
    }
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

extern "C" void v8_warmup(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    std::vector<uint8_t> storage;
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    v8::StartupData blob{nullptr, 0};
    int cause = INTERNAL_ERROR;
    char errbuf[512] = {0};
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request; // [snapshot, warmup_code]
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> blob_data_v;
        if (!request->Get(st.context, 0).ToLocal(&blob_data_v)) goto fail;
        v8::Local<v8::String> blob_data;
        if (!blob_data_v->ToString(st.context).ToLocal(&blob_data)) goto fail;
        assert(blob_data->IsOneByte());
        assert(blob_data->ContainsOnlyOneByte());
        if (const size_t len = blob_data->Length()) {
            auto flags = v8::String::NO_NULL_TERMINATION
                       | v8::String::PRESERVE_ONE_BYTE_NULL;
            storage.resize(len);
            blob_data->WriteOneByte(st.isolate, storage.data(), 0, len, flags);
        }
        v8::Local<v8::Value> code_v;
        if (!request->Get(st.context, 1).ToLocal(&code_v)) goto fail;
        v8::String::Utf8Value code(st.isolate, code_v);
        if (!*code) goto fail;
        auto data = reinterpret_cast<const char*>(storage.data());
        auto size = static_cast<int>(storage.size());
        v8::StartupData init{data, size};
        cause = snapshot(/*is_warmup*/true, st.verbose_exceptions, code, init, &blob, &errbuf);
        if (cause) goto fail;
    }
    if (blob.data) {
        auto data = reinterpret_cast<const uint8_t*>(blob.data);
        auto type = v8::NewStringType::kNormal;
        bool ok = v8::String::NewFromOneByte(st.isolate, data, type,
                                             blob.raw_size).ToLocal(&result);
        delete[] blob.data;
        blob = v8::StartupData{nullptr, 0};
        if (!ok) goto fail;
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
        // A watchdog/OOM termination preempting the reply supersedes any earlier
        // snapshot-failure message; drop it so we report the TERMINATED/MEMORY
        // error built from |cause| rather than the stale errbuf text.
        *errbuf = '\0';
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    v8::Local<v8::Value> err;
    if (*errbuf) {
        if (!v8::String::NewFromUtf8(st.isolate, errbuf).ToLocal(&err)) {
            err = v8::String::NewFromUtf8Literal(st.isolate, "unexpected error");
        }
    } else {
        err = to_error(st, &try_catch, cause);
    }
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

extern "C" void v8_low_memory_notification(State *pst)
{
    pst->isolate->LowMemoryNotification();
}

// called from ruby thread
extern "C" void v8_terminate_execution(State *pst)
{
    pst->isolate->TerminateExecution();
}

// Per-request realm restore for the dedicated V8 thread. v8_thread_init holds
// the Locker + Isolate::Scope for the thread's whole life, so unlike
// v8_single_threaded_enter this only needs a HandleScope. Re-deriving the
// Locals from the persistents on every request is what lets v8_reset_realm
// swap the realm underneath the running loop.
extern "C" void v8_threaded_enter(State *pst, Context *c, void (*f)(Context *c))
{
    State& st = *pst;
    v8::HandleScope handle_scope(st.isolate);
    restore_realm_locals(st);
    {
        v8::Context::Scope context_scope(st.context);
        f(c);
    }
    clear_realm_locals(st);
}

// Drops the current user realm and installs a fresh one from the (snapshot)
// default context, keeping the isolate — and its warm compilation cache —
// alive. The opt-in host namespace and any attached host functions are
// re-applied. Once install_realm commits the new realm and the old realm's
// remaining roots are released below, the previous globalThis (and everything
// hung off it) is unreachable and gets collected.
// Create a fresh realm (a new v8::Context in this isolate) and reply its
// integer id. The new realm inherits the attached host functions and the host
// namespace (install_realm re-binds them) and shares the isolate's security
// token, so per-frame realms can reach each other's globals. The caller's
// active realm is restored before returning.
extern "C" void v8_create_realm(State *pst)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    int cause = INTERNAL_ERROR;
    v8::Local<v8::Value> result;
    // Save the caller's active-realm Locals and restore them by assignment
    // afterward. install_realm clobbers st.context/safe_context/... while
    // building the new realm; re-deriving them with restore_realm_locals would
    // create handles in THIS function's HandleScope, leaving st.context
    // dangling once it unwinds. That is harmless at top-level (the per-request
    // enter clears + re-derives) but fatal when create_realm runs re-entrantly
    // from inside a host callback (the suspended outer frame resumes on the
    // dangling handle). The saved Locals' slots live in an outer, still-live
    // HandleScope, so restoring them keeps the caller valid either way.
    v8::Local<v8::Context> saved_context = st.context;
    v8::Local<v8::Context> saved_safe_context = st.safe_context;
    v8::Local<v8::Function> saved_safe_context_function = st.safe_context_function;
    int32_t prev = st.active_realm_id;
    int32_t id = st.next_realm_id;
    st.realms[id] = std::make_unique<Realm>();
    st.active_realm_id = id;
    bool built = install_realm(st); // builds + commits into realms[id]
    st.active_realm_id = prev;
    st.context = saved_context;
    st.safe_context = saved_safe_context;
    st.safe_context_function = saved_safe_context_function;
    if (built) {
        st.next_realm_id++;
        cause = NO_ERROR;
        result = v8::Integer::New(st.isolate, id);
    } else {
        st.realms.erase(id);        // build failed; drop the empty realm
    }
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (bubble_up_ruby_exception(st, &try_catch)) return;
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

// Dispose a realm created by v8_create_realm. Payload: [id:Int32]. Id 0 (the
// main realm) is never disposable. Clears the realm's handles under the live
// isolate, then nudges V8 to reclaim the detached context (as reset_realm does).
extern "C" void v8_dispose_realm(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::HandleScope handle_scope(st.isolate);

    // Refuse to erase a realm out from under a suspended JS->Ruby callback. A
    // host function (or dynamic-import / module-resolve callback) whose Ruby
    // code calls Realm#dispose leaves an outer frame entered in this isolate; if
    // that frame is running in the disposed realm, the resumed frame's
    // cur(st) = st.realms.at(active_realm_id) would abort under -fno-exceptions.
    // Same in_callback signal that guards v8_reset_realm. The realm is left
    // intact; realm_dispose surfaces this without marking the realm disposed.
    if (st.in_callback > 0) {
        refuse_in_callback(st, "dispose");
        return;
    }

    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> id_v;
    if (des.ReadValue(st.context).ToLocal(&id_v)) {
        int32_t id;
        if (id_v->Int32Value(st.context).To(&id) && id != 0) {
            auto it = st.realms.find(id);
            if (it != st.realms.end()) {
                Realm& r = *it->second;
                r.modules.clear();
                r.scripts.clear();
                r.module_id_by_url.clear();
                r.persistent_safe_context_function.Reset();
                r.persistent_safe_context.Reset();
                r.persistent_context.Reset();
                st.realms.erase(it);
                // Drop not-yet-delivered rejections tagged with this realm so
                // notify_unhandled_rejections never fires them against a gone
                // realm (or, if the id is later reused, the wrong one).
                for (auto pit = st.pending_rejections.begin(); pit != st.pending_rejections.end();) {
                    if (pit->second == id)
                        pit = st.pending_rejections.erase(pit);
                    else
                        ++pit;
                }
                st.isolate->ContextDisposedNotification(false);
            }
        }
    }
    reply_retry(st, v8::String::Empty(st.isolate));
}

// Run an op against a specific realm. Payload: [rid:int32][inner_op:1][inner
// request bytes]. Enters realm rid (active_realm_id + locals + Context::Scope)
// then delegates to the existing handler for inner_op, which replies as usual;
// the caller's active realm is restored afterward.
extern "C" void v8_realm_dispatch(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    int32_t rid = 0;
    if (n >= 4) memcpy(&rid, p, sizeof(rid));
    if (n < 5 || st.realms.find(rid) == st.realms.end()) {
        // Unknown/malformed realm — reply an error so the Ruby side doesn't
        // hang waiting for a response. The caller realm is still entered.
        v8::TryCatch try_catch(st.isolate);
        v8::HandleScope handle_scope(st.isolate);
        auto msg = v8::String::NewFromUtf8Literal(st.isolate, "no such realm");
        st.isolate->ThrowException(v8::Exception::Error(msg));
        auto err = to_error(st, &try_catch, RUNTIME_ERROR);
        reply_retry(st, err);
        return;
    }
    uint8_t op = p[4];
    const uint8_t *ip = p + 5;
    size_t in = n - 5;
    // Save the caller's active-realm Locals and restore them by assignment (not
    // restore_realm_locals, which would create dangling handles in this dying
    // HandleScope). Same hazard/fix as v8_create_realm: without this a re-entrant
    // realm op (e.g. Realm#eval called from a host function) leaves the resuming
    // outer frame on a dead st.context and SEGVs. The RAII Context::Scope below
    // already balances V8's entered-context stack; this restores the State Locals.
    v8::Local<v8::Context> saved_context = st.context;
    v8::Local<v8::Context> saved_safe_context = st.safe_context;
    v8::Local<v8::Function> saved_safe_context_function = st.safe_context_function;
    int32_t prev = st.active_realm_id;
    st.active_realm_id = rid;
    {
        v8::HandleScope handle_scope(st.isolate);
        restore_realm_locals(st);
        v8::Context::Scope context_scope(st.context);
        switch (op) {
        case 'E': v8_eval(pst, ip, in); break;
        case 'C': v8_call(pst, ip, in); break;
        case 'A': v8_attach(pst, ip, in); break;
        default:
            // Op not realm-scoped (yet). Reply an error rather than hang.
            v8::TryCatch try_catch(st.isolate);
            auto msg = v8::String::NewFromUtf8Literal(st.isolate,
                "operation is not supported on a realm");
            st.isolate->ThrowException(v8::Exception::Error(msg));
            auto err = to_error(st, &try_catch, RUNTIME_ERROR);
            reply_retry(st, err);
            break;
        }
    }
    st.active_realm_id = prev;
    st.context = saved_context;
    st.safe_context = saved_safe_context;
    st.safe_context_function = saved_safe_context_function;
}

extern "C" void v8_reset_realm(State *pst)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);

    // Refuse to swap the realm out from under a JS->Ruby callback. When a host
    // function's Ruby code calls reset_realm, an outer v8_api_callback frame is
    // suspended mid-roundtrip with the old context entered and would resume
    // against the swapped realm — corrupting it. (in_callback is the same signal
    // that makes compile() refuse CreateCodeCache mid-callback.)
    if (st.in_callback > 0) {
        refuse_in_callback(st, "reset_realm");
        return;
    }

    // install_realm is all-or-nothing: on failure the previous realm is left
    // intact in the persistents. Surface the cause, and if a watchdog/OOM
    // terminated execution mid-rebuild, clear it here so it does not poison the
    // next unrelated request (mirrors v8_eval/v8_call's termination handling).
    if (!install_realm(st)) {
        int cause;
        if (st.isolate->IsExecutionTerminating()) {
            st.isolate->CancelTerminateExecution();
            cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
            st.err_reason = NO_ERROR;
        } else {
            cause = try_catch.HasCaught() ? RUNTIME_ERROR : INTERNAL_ERROR;
        }
        // Restore Locals (the rebuild may have cleared them) so the error reply
        // can be serialized against the surviving realm.
        restore_realm_locals(st);
        reply_retry(st, to_error(st, &try_catch, cause));
        return;
    }

    // New realm committed. Release the remaining roots into the old realm:
    // a pending ruby-exception handle and the scripts/modules compiled against
    // it (their handles are realm-bound, so they would both pin the old realm
    // and, if run after the swap, execute against the wrong globalThis — they
    // are invalidated here).
    if (module_trace_on())
        fprintf(stderr, "[mr.reset] clearing modules=%zu scripts=%zu url_index=%zu\n",
                cur(st).modules.size(), cur(st).scripts.size(), cur(st).module_id_by_url.size()), fflush(stderr);
    st.ruby_exception.Reset();
    // clear() destroys each v8::Global, and ~Global() Resets the handle under
    // the still-live isolate — no explicit Reset loop needed (it would be for
    // the old default-traits Persistent, whose destructor is a no-op).
    cur(st).scripts.clear();
    cur(st).modules.clear();
    cur(st).module_id_by_url.clear();
    // The realm-0 struct is reused across reset, so its onUnhandledRejection
    // handler points at a function in the now-discarded old realm. Drop it; the
    // embedder re-registers on the fresh realm (the namespace is reinstalled).
    cur(st).unhandled_rejection_handler.Reset();
    // Same rationale as the scripts/modules above: a not-yet-delivered rejection
    // recorded against the old realm would, after the swap, fire against the
    // fresh realm's globalThis (reset reuses the realm id). Drop them.
    for (auto pit = st.pending_rejections.begin(); pit != st.pending_rejections.end();) {
        if (pit->second == st.active_realm_id)
            pit = st.pending_rejections.erase(pit);
        else
            ++pit;
    }
    // The fresh realm has no registered modules; fall back to the legacy dynamic
    // import resolver until load_module_graph runs again and re-latches this.
    cur(st).uses_graph_loader = false;

    // Tell V8 the old realm is gone — the same primitive a browser uses on
    // navigation / iframe teardown. Unlike low_memory_notification (a full GC
    // that also flushes the compilation cache, throwing away the warm bytecode
    // that makes realm reuse worthwhile), this nudges V8's MemoryReducer to
    // reclaim the now-detached realm incrementally while KEEPING the compilation
    // cache. Without it, detached realms accrue until heap pressure forces a GC,
    // which on a heavy app (e.g. Discourse) can overshoot the limit before it
    // fires. The `false` (no dependent context) argument is what triggers the
    // proactive reduction — mirrors the snapshot-warmup call site.
    st.isolate->ContextDisposedNotification(false);

    // Restore Locals from the new persistents and enter the new context so the
    // reply is serialized against it (install_realm leaves the members empty).
    restore_realm_locals(st);
    v8::Context::Scope context_scope(st.context);
    reply_retry(st, to_error(st, &try_catch, NO_ERROR));
}

extern "C" void v8_single_threaded_enter(State *pst, Context *c, void (*f)(Context *c))
{
    State& st = *pst;
    v8::Locker locker(st.isolate);
    v8::Isolate::Scope isolate_scope(st.isolate);
    v8::HandleScope handle_scope(st.isolate);
    {
        restore_realm_locals(st);
        v8::Context::Scope context_scope(st.context);
        f(c);
        clear_realm_locals(st);
    }
}

extern "C" void v8_single_threaded_dispose(struct State *pst)
{
    delete pst; // see State::~State() below
}

extern "C" uint32_t v8_cached_data_version_tag(void)
{
    return v8::ScriptCompiler::CachedDataVersionTag();
}

} // namespace anonymous

State::~State()
{
    {
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolate_scope(isolate);
        // Clear every realm's handles under the still-live isolate so each
        // Global Reset()s before isolate->Dispose().
        for (auto& kv : realms) {
            Realm& r = *kv.second;
            r.modules.clear();
            r.scripts.clear();
            r.unhandled_rejection_handler.Reset();
            r.persistent_safe_context_function.Reset();
            r.persistent_safe_context.Reset();
            r.persistent_context.Reset();
        }
        pending_rejections.clear(); // Global<Promise> dtors Reset under the live isolate
        realms.clear();
        ruby_exception.Reset();
        shared_security_token.Reset();
    }
    isolate->Dispose();
    for (Callback *cb : callbacks)
        delete cb;
}
