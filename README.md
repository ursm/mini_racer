# MiniRacer

[![Test](https://github.com/rubyjs/mini_racer/actions/workflows/ci.yml/badge.svg)](https://github.com/rubyjs/mini_racer/actions/workflows/ci.yml) ![Gem](https://img.shields.io/gem/v/mini_racer)

Minimal, modern embedded V8 for Ruby.

MiniRacer provides a minimal two way bridge between the V8 JavaScript engine and Ruby.

It was created as an alternative to the excellent [therubyracer](https://github.com/cowboyd/therubyracer), which is [no longer maintained](https://github.com/rubyjs/therubyracer/issues/462). Unlike therubyracer, mini_racer only implements a minimal bridge. This reduces the surface area making upgrading v8 much simpler and exhaustive testing simpler.

MiniRacer has an adapter for [execjs](https://github.com/rails/execjs) so it can be used directly with Rails projects to minify assets, run babel or compile CoffeeScript.

## This repository is `mini_racer-csim` (a fork)

This is **`mini_racer-csim`**, a private fork of [`mini_racer`](https://github.com/rubyjs/mini_racer) maintained for [capybara-simulated](https://github.com/ursm/capybara-simulated). It adds browser-fidelity extensions (ES modules, per-frame realms, realm reset, …) that capybara-simulated needs but most users do not — **if you are not using capybara-simulated, use upstream `mini_racer`.**

It has its **own require path and namespace** so it never collides with — and loads deterministically alongside — upstream `mini_racer` in the same bundle: load it with `require 'mini_racer_csim'` and use the `MiniRacerCsim` module (e.g. `MiniRacerCsim::Context`). The native extensions are `mini_racer_csim_extension` / `mini_racer_csim_loader`. (The JS-side host-namespace brand, `globalThis.MiniRacer` by default, is embedder-chosen and unrelated to the Ruby namespace.)

### Additions over upstream

| Feature | API | Notes |
| --- | --- | --- |
| Bytecode cache | `Context#compile(src, cached_data:, produce_cache:)` → `Script`, `Script#run`, `Script#cache_rejected?` | Cross-process V8 bytecode caching to skip parsing; see [Bytecode cache for repeated script evaluation](#bytecode-cache-for-repeated-script-evaluation) below |
| ES Module API | `Context#compile_module` → `MiniRacerCsim::Module` (`#instantiate` / `#evaluate` / `#namespace` / `#status` / `#cached_data` / `#dispose`); `Context#dynamic_import_resolver=` | V8's ES module pipeline, `import.meta.url`, dynamic `import()` |
| Batched module-graph loader | `Context#load_module_graph(resolve:, …)` | Loads an ESM graph in one batched, native (C++) pass; one `Module` per URL shared across every load path |
| Realm reset | `Context#reset_realm` | Discards the user realm (`globalThis`) while keeping the warm isolate (browser per-navigation model); re-binds attached host functions and the host namespace |
| Per-frame realms | `Context#create_realm` → `MiniRacerCsim::Realm` (`#eval` / `#call` / `#attach` / `#dispose` / `#disposed?`) | Multiple V8 realms (Contexts) in one isolate with browser-iframe semantics: realms share one security token for cross-realm access. JS-side helpers live on the [host namespace](#host-namespace) (so using them needs `host_namespace:`): `<ns>.realmGlobal(id)` exposes a realm's live `globalThis`, `<ns>.realmOf(fn)` reports a callback's `[[Realm]]`, and `<ns>.onUnhandledRejection(fn)` registers a per-realm unhandled-rejection handler. Realms themselves work without the namespace (driven from Ruby). |
| Host namespace | `Context.new(host_namespace: "MiniRacer")` → `globalThis.MiniRacer.drainMicrotasks()` | Opt-in JS namespace exposing an inline, rendezvous-free microtask checkpoint |
| GVL release on boot | (automatic) | Releases the Ruby GVL while the V8 thread boots the isolate |

This is a hard fork: it no longer tracks upstream `mini_racer`, and follows only `libv8-node` for V8 version bumps.

## Supported Ruby Versions & Troubleshooting

MiniRacer only supports non-EOL versions of Ruby. See [Ruby Maintenance Branches](https://www.ruby-lang.org/en/downloads/branches/) for the list of non-EOL Rubies. If you require support for older versions of Ruby install an older version of the gem. (The `mini_racer-csim` fork is CRuby/V8 only — the TruffleRuby backend that upstream supports has been removed.)

MiniRacer **does not support**

* [Ruby built on MinGW](https://github.com/rubyjs/mini_racer/issues/252#issuecomment-1201172236), "pure windows" no Cygwin, no WSL2 (see https://github.com/rubyjs/libv8-node/issues/9)
* [JRuby](https://www.jruby.org)

If you have a problem installing MiniRacer, please consider the following steps:

* make sure you try the latest released version of `mini_racer`
* make sure you have Rubygems >= 3.2.13 and bundler >= 2.2.13 installed: `gem update --system`
* if you are using bundler
  * make sure it is actually using the latest bundler version: [`bundle update --bundler`](https://bundler.io/v2.4/man/bundle-update.1.html)
  * make sure to have `PLATFORMS` set correctly in `Gemfile.lock` via [`bundle lock --add-platform`](https://bundler.io/v2.4/man/bundle-lock.1.html#SUPPORTING-OTHER-PLATFORMS)
* make sure to recompile/reinstall `mini_racer` and `libv8-node` after OS upgrades (for example via `gem uninstall --all mini_racer libv8-node`)
* make sure you are on the latest patch/teeny version of a supported Ruby branch

## Features

### Simple eval for JavaScript

You can simply eval one or many JavaScript snippets in a shared context

```ruby
context = MiniRacerCsim::Context.new
context.eval("var adder = (a,b)=>a+b;")
puts context.eval("adder(20,22)")
# => 42
```

### Attach global Ruby functions to your JavaScript context

You can attach one or many ruby proc that can be accessed via JavaScript

```ruby
context = MiniRacerCsim::Context.new
context.attach("math.adder", proc{|a,b| a+b})
puts context.eval("math.adder(20,22)")
# => 42
```

```ruby
context = MiniRacerCsim::Context.new
context.attach("array_and_hash", proc{{a: 1, b: [1, {a: 1}]}})
puts context.eval("array_and_hash()")
# => {"a" => 1, "b" => [1, {"a" => 1}]}
```

### Return binary data from Ruby to JavaScript

Attached Ruby functions can return binary data as `Uint8Array` using `MiniRacerCsim::Binary`:

```ruby
require "digest"

context = MiniRacerCsim::Context.new
context.attach("sha256_raw", ->(data) {
  MiniRacerCsim::Binary.new(Digest::SHA256.digest(data))
})

# Inside JavaScript the return value is a Uint8Array
context.eval("sha256_raw('hello') instanceof Uint8Array") # => true
context.eval("sha256_raw('hello').length")                 # => 32
```

This is useful when you need to pass raw bytes (e.g., cryptographic digests, compressed data, binary file contents) from Ruby to JavaScript. The `MiniRacerCsim::Binary` wrapper tells the bridge to serialize the data as a `Uint8Array` on the JavaScript side rather than a string.

### GIL free JavaScript execution

The Ruby Global interpreter lock is released when scripts are executing:

```ruby
context = MiniRacerCsim::Context.new
Thread.new do
  sleep 1
  context.stop
end
context.eval("while(true){}")
# => exception is raised
```

This allows you to execute multiple scripts in parallel.

### Timeout Support

Contexts can specify a default timeout for scripts

```ruby
context = MiniRacerCsim::Context.new(timeout: 1000)
context.eval("while(true){}")
# => exception is raised after 1 second (1000 ms)
```

### Memory softlimit Support

Contexts can specify a memory softlimit for scripts

```ruby
# terminates script if heap usage exceeds 200mb after V8 garbage collection has run
context = MiniRacerCsim::Context.new(max_memory: 200_000_000)
context.eval("var a = new Array(10000); while(true) {a = a.concat(new Array(10000)) }")
# => V8OutOfMemoryError is raised
```

### Rich Debugging with File Name in Stack Trace Support

You can provide `filename:` to `#eval` which will be used in stack traces produced by V8:

```ruby
context = MiniRacerCsim::Context.new
context.eval("var foo = function() {bar();}", filename: "a/foo.js")
context.eval("bar()", filename: "a/bar.js")

# JavaScript at a/bar.js:1:1: ReferenceError: bar is not defined (MiniRacerCsim::RuntimeError)
# …
```

### Bytecode cache for repeated script evaluation

`Context#compile` returns a `MiniRacerCsim::Script` handle you can run multiple times,
and exposes V8's bytecode cache so subsequent Contexts can skip the parse step.

In a single process — e.g. warming a `Context` pool from one canonical compile:

```ruby
# Warm the cache once — top-level compile, opt in with produce_cache: true.
warm    = MiniRacerCsim::Context.new
warmed  = warm.compile(File.read("bundle.js"),
                       filename:      "bundle.js",
                       produce_cache: true)
warmed.run
blob = warmed.cached_data   # ASCII-8BIT String, hold onto it in memory

# Subsequent Contexts (e.g. a per-request pool) consume the blob and skip parsing.
ctx    = MiniRacerCsim::Context.new
script = ctx.compile(File.read("bundle.js"),
                     filename:    "bundle.js",
                     cached_data: blob)
# script.cache_rejected? is false when V8 accepted the blob.
script.run
```

Across processes (e.g. persisting blobs to disk), the consumer must boot from
**byte-identical snapshot data** — two separate `Snapshot.new(src)` calls produce
different blobs even for the same `src`, and V8 will then reject every cached
blob. Use `Snapshot#dump` / `Snapshot.load` to share canonical bytes:

```ruby
# Build the snapshot once, persist its bytes.
snap_bytes = MiniRacerCsim::Snapshot.new(snapshot_src).dump
File.binwrite("snapshot.bin", snap_bytes)

# Every process loads the same bytes.
snap = MiniRacerCsim::Snapshot.load(File.binread("snapshot.bin"))
ctx  = MiniRacerCsim::Context.new(snapshot: snap)
script = ctx.compile(File.read("bundle.js"),
                     filename:    "bundle.js",
                     cached_data: File.binread("bundle.js.cache"))
script.run
```

`produce_cache` defaults to `false`; pass `true` to ask V8 for the cache blob.
When the supplied `cached_data` is accepted, `script.cached_data` returns `nil` so
callers can skip a redundant copy. When V8 produces a fresh blob (initial compile
with `produce_cache: true`, or a rejection while `produce_cache: true` was also
set), it returns the new bytes.

`MiniRacerCsim::V8_CACHED_DATA_VERSION_TAG` exposes V8's
`ScriptCompiler::CachedDataVersionTag()` — mix it into your cache key alongside
the source hash so a libv8-node version bump invalidates stale blobs automatically.
The constant is populated on first `Context.new` (after `Platform.set_flags!`),
so read it after constructing at least one Context.

```ruby
key = "#{Digest::SHA256.hexdigest(source)}-#{MiniRacerCsim::V8_CACHED_DATA_VERSION_TAG}"
```

Notes:

- A `Script` is bound to the `Context` that compiled it; reusing it on another
  Context isn't supported.
- `Script#dispose` frees the underlying V8 handle eagerly. The Ruby GC finalizer
  does not (taking the V8 lock from a finalizer thread risks deadlock), so
  long-lived Contexts with many short-lived scripts accumulate handles until
  `Context#dispose` clears them.
- `produce_cache: true` is only safe at the top level. From inside a host-fn
  callback (i.e., re-entrant compile while a JS → Ruby → JS frame is on the
  stack) it raises `MiniRacerCsim::RuntimeError`, because V8's `CreateCodeCache`
  walks live isolate state and corrupts the parser when re-entered. Warm the
  cache from the top level once and pass it back via `cached_data:` from your
  callbacks.
- Cross-process reuse is **incompatible with `MiniRacerCsim::Platform.set_flags!(:single_threaded)`**.
  V8's single-threaded mode embeds process-local state in the cache blob, so
  every cached_data is rejected when consumed in a fresh process. Same-process
  reuse still works under `:single_threaded`. If you need both cross-process
  reuse and `:single_threaded` (e.g. for fork-safety reasons), disable
  `:single_threaded` for the path that produces / consumes the cache.

### Fork Safety

Some Ruby web servers employ forking (for example unicorn or puma in clustered mode). V8 is not fork safe by default and sadly Ruby does not have support for fork notifications per [#5446](https://bugs.ruby-lang.org/issues/5446).

Since 0.6.1 mini_racer does support V8 single threaded platform mode which should remove most forking related issues. To enable run this before using `MiniRacerCsim::Context`, for example in a Rails initializer:

```ruby
MiniRacerCsim::Platform.set_flags!(:single_threaded)
```

When using pre-fork `MiniRacerCsim::Context` objects in `:single_threaded` mode,
ensure the process only forks while MiniRacer is quiescent: no thread may be
evaluating JavaScript, calling into a context, disposing/freeing a context,
running a Ruby callback from JavaScript, or otherwise using MiniRacer at the
instant of `fork`. In multi-threaded applications, guard all MiniRacer context
operations and the `fork` itself with the same application-level lock. Forking
while a MiniRacer operation is in progress can leave inherited pthread mutexes
in an unusable state in the child process.

If you want to ensure your application does not leak memory after fork either:

1. Ensure no `MiniRacerCsim::Context` objects are created in the master process; or
2. Dispose manually of all `MiniRacerCsim::Context` objects prior to forking

```ruby
# before fork

require "objspace"
ObjectSpace.each_object(MiniRacerCsim::Context){|c| c.dispose}

# fork here
```

### Threadsafe

Context usage is threadsafe

```ruby
context = MiniRacerCsim::Context.new
context.eval("counter=0; plus=()=>counter++;")

(1..10).map do
  Thread.new {
    context.eval("plus()")
  }
end.each(&:join)

puts context.eval("counter")
# => 10
```

### Snapshots

Contexts can be created with pre-loaded snapshots:

```ruby
snapshot = MiniRacerCsim::Snapshot.new("function hello() { return 'world!'; }")

context = MiniRacerCsim::Context.new(snapshot: snapshot)

context.eval("hello()")
# => "world!"
```

Snapshots can come in handy for example if you want your contexts to be pre-loaded for efficiency. It uses [V8 snapshots](http://v8project.blogspot.com/2015/09/custom-startup-snapshots.html) under the hood; see [this link](http://v8project.blogspot.com/2015/09/custom-startup-snapshots.html) for caveats using these, in particular:

> There is an important limitation to snapshots: they can only capture V8’s
> heap. Any interaction from V8 with the outside is off-limits when creating the
> snapshot. Such interactions include:
>
> * defining and calling API callbacks (i.e. functions created via v8::FunctionTemplate)
> * creating typed arrays, since the backing store may be allocated outside of V8
>
> And of course, values derived from sources such as `Math.random` or `Date.now`
> are fixed once the snapshot has been captured. They are no longer really random
> nor reflect the current time.

Also note that snapshots can be warmed up, using the `warmup!` method, which allows you to call functions which are otherwise lazily compiled to get them to compile right away; any side effect of your warm up code being then dismissed. [More details on warming up here](https://github.com/electron/electron/issues/169#issuecomment-76783481), and a small example:

```ruby
snapshot = MiniRacerCsim::Snapshot.new("var counter = 0; function hello() { counter++; return 'world! '; }")

snapshot.warmup!("hello()")

context = MiniRacerCsim::Context.new(snapshot: snapshot)

context.eval("hello()")
# => "world! 1"
context.eval("counter")
# => 1
```

Snapshots can also be persisted to disk for faster startup:

```ruby
# Save a snapshot to disk
snapshot = MiniRacerCsim::Snapshot.new('var foo = "bar";')
File.binwrite("snapshot.bin", snapshot.dump)

# Load it back in a later process
blob = File.binread("snapshot.bin")
snapshot = MiniRacerCsim::Snapshot.load(blob)
context = MiniRacerCsim::Context.new(snapshot: snapshot)
context.eval("foo")
# => "bar"
```

Note that snapshots are architecture and V8-version specific. A snapshot created on one platform (e.g., ARM64 macOS) cannot be loaded on a different platform (e.g., x86_64 Linux). Snapshots are best used for same-machine caching or homogeneous deployment environments.

**Security note:** Only load snapshots from trusted sources. V8 snapshots are not designed to be safely loaded from untrusted input—malformed or malicious snapshot data may cause crashes or memory corruption.

### Garbage collection

You can make the garbage collector more aggressive by defining the context with `MiniRacerCsim::Context.new(ensure_gc_after_idle: 1000)`. Using this will ensure V8 will run a full GC using `context.low_memory_notification` 1 second after the last eval on the context. Low memory notifications ensure long living contexts use minimal amounts of memory.

### V8 Runtime flags

It is possible to set V8 Runtime flags:

```ruby
MiniRacerCsim::Platform.set_flags! :noconcurrent_recompilation, max_inlining_levels: 10
```

This can come in handy if you want to use MiniRacer with Unicorn, which doesn't seem to always appreciate V8's liberal use of threading:

```ruby
MiniRacerCsim::Platform.set_flags! :noconcurrent_recompilation, :noconcurrent_sweeping
```

Or else to unlock experimental features in V8, for example tail recursion optimization:

```ruby
MiniRacerCsim::Platform.set_flags! :harmony

js = <<-JS
'use strict';
var f = function f(n){
  if (n <= 0) {
    return  'foo';
  }
  return f(n - 1);
}

f(1e6);
JS

context = MiniRacerCsim::Context.new

context.eval js
# => "foo"
```

The same code without the harmony runtime flag results in a `MiniRacerCsim::RuntimeError: RangeError: Maximum call stack size exceeded` exception.
Please refer to http://node.green/ as a reference on other harmony features.

A list of all V8 runtime flags can be found using `node --v8-options`, or else by perusing [the V8 source code for flags (make sure to use the right version of V8)](https://github.com/v8/v8/blob/master/src/flags/flag-definitions.h).

Note that runtime flags must be set before any other operation (e.g. creating a context or a snapshot), otherwise an exception will be thrown.

Flags:

* `:expose_gc`: Will expose `gc()` which you can run in JavaScript to issue a GC run.
* `:max_old_space_size`: defaults to 1400 (megs) on 64 bit, you can restrict memory usage by limiting this.

**NOTE TO READER** our documentation could be awesome we could be properly documenting all the flags, they are hugely useful, if you feel like documenting a few more, PLEASE DO, PRs are welcome.

## Controlling memory

When hosting v8 you may want to keep track of memory usage, use `#heap_stats` to get memory usage:

```ruby
context = MiniRacerCsim::Context.new
# use context
p context.heap_stats
# {:total_physical_size=>1280640,
#  :total_heap_size_executable=>4194304,
#  :total_heap_size=>3100672,
#  :used_heap_size=>1205376,
#  :heap_size_limit=>1501560832}
```

If you wish to dispose of a context before waiting on the GC use `#dispose`:

```ruby
context = MiniRacerCsim::Context.new
context.eval("let a='testing';")
context.dispose
context.eval("a = 2")
# MiniRacerCsim::ContextDisposedError

# nothing works on the context from now on, it's a shell waiting to be disposed
```

A MiniRacer context can also be dumped in a heapsnapshot file using `#write_heap_snapshot(file_or_io)`

```ruby
context = MiniRacerCsim::Context.new
# use context
context.write_heap_snapshot("test.heapsnapshot")
```

This file can then be loaded in the "memory" tab of the [Chrome DevTools](https://developer.chrome.com/docs/devtools/memory-problems/heap-snapshots/#view_snapshots).

### Function call

This calls the function passed as first argument:

```ruby
context = MiniRacerCsim::Context.new
context.eval("function hello(name) { return `Hello, ${name}!` }")
context.call("hello", "George")
# "Hello, George!"
```

Performance is slightly better than running `context.eval("hello('George')")` since:

* compilation of eval'd string is avoided
* function arguments don't need to be converted to JSON

### Microtask checkpoints

V8 drains its microtask queue (e.g. callbacks queued via `Promise.resolve().then(...)`) automatically when script execution returns to the embedder, so most code "just works":

```ruby
context = MiniRacerCsim::Context.new
context.eval(<<~JS)
  let x = 0;
  Promise.resolve().then(() => x = 99);
JS
context.eval("x")
# => 99
```

When JavaScript invokes a Ruby callback synchronously and you need queued microtasks to drain mid-execution — e.g. for spec-compliant ordering across a chain of synchronous `dispatchEvent` listeners — call `context.perform_microtask_checkpoint` from the callback:

```ruby
context = MiniRacerCsim::Context.new
context.attach("drain", -> { context.perform_microtask_checkpoint })
context.eval(<<~JS)
  globalThis.log = [];
  Promise.resolve().then(() => log.push("microtask"));
  log.push("before");
  drain();
  log.push("after");
JS
context.eval("log")
# => ["before", "microtask", "after"]
```

Without `drain()` the order would be `["before", "after", "microtask"]` because the microtask only runs once the outermost script returns. `perform_microtask_checkpoint` is a thin wrapper over V8's `MicrotasksScope::PerformCheckpoint`.

When the drain has to happen from within JavaScript itself — for example between each listener in a synchronous `dispatchEvent` chain — the same checkpoint is available to JS as `drainMicrotasks()`. It runs inline on the V8 thread without the Ruby ↔ V8 round-trip, so no `attach` is required.

### Host namespace

`mini_racer-csim` exposes its non-standard JS-callable helpers through a single opt-in object (in the spirit of Deno's `Deno` or Bun's `Bun`) rather than as bare globals, so allowing them is a one-time decision and `globalThis` otherwise stays clean. Pass `host_namespace:` to enable it; by default nothing is injected:

```ruby
context = MiniRacerCsim::Context.new(host_namespace: "MiniRacer")
context.eval(<<~JS)
  globalThis.log = [];
  Promise.resolve().then(() => log.push("microtask"));
  log.push("before");
  MiniRacer.drainMicrotasks();
  log.push("after");
JS
context.eval("log")
# => ["before", "microtask", "after"]
```

Beyond `drainMicrotasks()`, the namespace also carries the per-frame-realm JS helpers — `realmGlobal(id)`, `realmOf(fn)`, and `onUnhandledRejection(fn)` (see the **Per-frame realms** entry above). They live here for the same reason, so cross-realm wiring *in JS* requires `host_namespace:`; realms driven from Ruby (`Context#create_realm` + `Realm#eval`/`call`) do not.

`host_namespace:` accepts a String (the global name to use — it must be a valid JavaScript identifier), `true` (the default name `"MiniRacer"`), or `nil`/`false` (the default — inject nothing). The namespace object is defined non-enumerable so it does not appear in `Object.keys(globalThis)`, while its methods are ordinary properties discoverable via `Object.keys(MiniRacer)`. Like `perform_microtask_checkpoint`, `drainMicrotasks()` is a no-op while a microtask checkpoint is already in progress, and it lets watchdog/out-of-memory termination propagate to the enclosing `eval`/`call`.

### ES modules

`Context#compile_module` exposes V8's ES module API for code that uses
`import` / `export` syntax. Unlike `eval` (which only accepts script-level
syntax), modules can have static imports that resolve to other modules and
expose named exports through a real Module Namespace Object.

```ruby
context = MiniRacerCsim::Context.new

dep  = context.compile_module("export const base = 10", filename: "dep.js")
main = context.compile_module(<<~JS, filename: "main.js")
  import { base } from 'dep'
  export const doubled = base * 2
JS

main.instantiate {|specifier, referrer| dep }  # called once per static import
dep.evaluate
main.evaluate

main.namespace  # => {"doubled" => 20}
```

* `Context#compile_module(source, filename:)` — parses the source as a
  module; the returned `MiniRacerCsim::Module` is bound to its Context. The
  `filename` is also exposed to the module as `import.meta.url`.
* `Module#instantiate { |specifier, referrer_url| ... }` — walks the static
  import graph. The resolver block is called once per import declaration
  with the raw specifier string and the importing module's filename, so
  relative specifiers (`./foo`, `../bar`) can be resolved against the
  referrer. It must return another `MiniRacerCsim::Module` (typically from a
  per-Context cache). Imports can also be resolved lazily from inside the
  block via further `Context#compile_module` calls.
* `Module#evaluate` — runs the module body. Returns the evaluation result
  (`nil` for the typical `export const …` shape). Modules with top-level
  `await` raise `MiniRacerCsim::RuntimeError` for now.
* `Module#namespace` — returns the Module Namespace Object as a Hash
  (`{ "default" => …, "namedExport" => … }`). Available after
  `instantiate` succeeds; `evaluate` populates the values.
* `Module#status` — one of `:uninstantiated`, `:instantiating`,
  `:instantiated`, `:evaluating`, `:evaluated`, `:errored`.
* `Module#dispose` / `Module#disposed?` — eager handle release, mirroring
  the convention used elsewhere.
* `Context#dynamic_import_resolver = proc { |specifier, referrer_url| ... }`
  — handler for JS `import(...)` expressions. The proc must return a
  `MiniRacerCsim::Module` (already instantiated; `evaluate` is driven for you
  if pending). Set to `nil` to reject all dynamic imports. Drain the
  microtask queue with `Context#perform_microtask_checkpoint` to see the
  result in a `.then` callback or after `await`.

```ruby
context.dynamic_import_resolver = ->(spec, _ref) { cache.fetch(spec) }
context.eval(%(import('dep').then(ns => globalThis.r = ns.x)), filename: 'caller.js')
context.perform_microtask_checkpoint
context.eval('globalThis.r')  # => 42
```

Notes:

- A `Module` is bound to the `Context` that compiled it; resolvers must
  return modules from the same Context.
- `Module#dispose` frees the underlying V8 handle eagerly. The Ruby GC
  finalizer does not (taking the V8 lock from a finalizer thread risks
  deadlock), so long-lived Contexts with many short-lived modules
  accumulate handles until `Context#dispose` clears them.
- Top-level await is not yet supported; `evaluate` raises if the
  module's evaluation promise stays pending after the microtask drain.

## Performance

The `bench` folder contains benchmark.

### Benchmark minification of Discourse application.js (both minified and non-minified)

MiniRacer outperforms node when minifying assets via execjs.

* MiniRacer version 0.1.9
* node version 6.10
* therubyracer version 0.12.2

```terminal
$ bundle exec ruby bench.rb mini_racer
Benching with mini_racer
mini_racer minify discourse_app.js 9292.72063ms
mini_racer minify discourse_app_minified.js 11799.850171ms
mini_racer minify discourse_app.js twice (2 threads) 10269.570797ms

sam@ubuntu exec_js_uglify % bundle exec ruby bench.rb node
Benching with node
node minify discourse_app.js 13302.715484ms
node minify discourse_app_minified.js 18100.761243ms
node minify discourse_app.js twice (2 threads) 14383.600207000001ms

sam@ubuntu exec_js_uglify % bundle exec ruby bench.rb therubyracer
Benching with therubyracer
therubyracer minify discourse_app.js 171683.01867700001ms
therubyracer minify discourse_app_minified.js 143138.88492ms
therubyracer minify discourse_app.js twice (2 threads) NEVER FINISH

Killed: 9
```

The huge performance disparity (MiniRacer is 10x faster) is due to MiniRacer running latest version of V8. In July 2016 there is a queued upgrade to therubyracer which should bring some of the perf inline.

Note how the global interpreter lock release leads to 2 threads doing the same work taking the same wall time as 1 thread.

As a rule MiniRacer strives to always support and depend on the latest stable version of libv8.

## Source Maps

MiniRacer can fully support source maps but must be configured correctly to do so. [Check out this example](./examples/source-map-support/) for a working implementation.

## Installation

Add this line to your application's Gemfile:

```ruby
gem "mini_racer"
```

And then execute:

```terminal
$ bundle

Or install it yourself as:

```terminal
$ gem install mini_racer
```

**Note** using v8.h and compiling MiniRacer requires a C++20 capable compiler.
gcc >= 12.2 and Xcode >= 13 are, at the time of writing, known to work.

## Similar Projects

### therubyracer

* https://github.com/cowboyd/therubyracer
* Most comprehensive bridge available
* Provides the ability to "eval" JavaScript
* Provides the ability to invoke Ruby code from JavaScript
* Hold references to JavaScript objects and methods in your Ruby code
* Hold references to Ruby objects and methods in JavaScript code
* Uses libv8, so installation is fast
* Supports timeouts for JavaScript execution
* Does not release global interpreter lock, so performance is constrained to a single thread
* Currently (May 2016) only supports v8 version 3.16.14 (Released approx November 2013), plans to upgrade by July 2016
* Supports execjs

### v8eval

* https://github.com/sony/v8eval
* Provides the ability to "eval" JavaScript using the latest V8 engine
* Does not depend on the [libv8](https://github.com/cowboyd/libv8) gem, installation can take 10-20 mins as V8 needs to be downloaded and compiled.
* Does not release global interpreter lock when executing JavaScript
* Does not allow you to invoke Ruby code from JavaScript
* Multi runtime support due to SWIG based bindings
* Supports a JavaScript debugger
* Does not support timeouts for JavaScript execution
* No support for execjs (can not be used with Rails uglifier and coffeescript gems)

### therubyrhino

* https://github.com/cowboyd/therubyrhino
* API compatible with therubyracer
* Uses Mozilla's Rhino engine https://github.com/mozilla/rhino
* Requires JRuby
* Support for timeouts for JavaScript execution
* Concurrent cause .... JRuby
* Supports execjs

## Contributing

Bug reports and pull requests are welcome on GitHub at https://github.com/rubyjs/mini_racer. This project is intended to be a safe, welcoming space for collaboration, and contributors are expected to adhere to the [Contributor Covenant](http://contributor-covenant.org) code of conduct.

## License

The gem is available as open source under the terms of the [MIT License](http://opensource.org/licenses/MIT).
