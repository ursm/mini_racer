# frozen_string_literal: true

require "test_helper"
require "rbconfig"
require "tempfile"
require "timeout"

class MiniRacerSingleThreadedTest < Minitest::Test
  def assert_single_threaded_script(script, timeout: 60)
    skip "single-threaded V8 platform tests are only for CRuby" unless RUBY_ENGINE == "ruby"

    file = Tempfile.new(["mini_racer_single_threaded", ".rb"])
    file.write(<<~RUBY)
      $LOAD_PATH.unshift #{File.expand_path("../lib", __dir__).inspect}
      require 'mini_racer_csim'

      MiniRacerCsim::Platform.set_flags!(:single_threaded)

      #{script}
    RUBY
    file.close

    # Run with a bounded wait and kill on timeout so a regression that
    # deadlocks the single-threaded runner (e.g. a v8::Locker taken on the
    # shared Ruby thread) fails deterministically instead of hanging the suite.
    read, write = IO.pipe
    pid = Process.spawn(RbConfig.ruby, file.path, out: write, err: write)
    write.close
    reader = Thread.new { read.read }

    begin
      _, status = Timeout.timeout(timeout) { Process.wait2(pid) }
    rescue Timeout::Error
      Process.kill("KILL", pid)
      Process.wait(pid)
      flunk "single-threaded script did not finish within #{timeout}s (possible deadlock):\n#{reader.value}"
    end

    output = reader.value
    read.close
    assert status.success?, <<~MSG
      single-threaded script failed with status #{status.exitstatus}
      output:
      #{output}
    MSG
  ensure
    file&.unlink
  end

  def test_basic_eval_and_call
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      raise "bad eval" unless context.eval("1 + 1") == 2
      context.eval("function add(a, b) { return a + b }")
      raise "bad call" unless context.call("add", 20, 22) == 42
    RUBY
  end

  def test_ruby_callback_from_javascript
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      context.attach("ruby_add", proc { |a, b| a + b })
      raise "bad callback result" unless context.eval("ruby_add(20, 22)") == 42
    RUBY
  end

  def test_nested_javascript_ruby_javascript_call
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      context.eval("function js_add(a, b) { return a + b }")
      context.attach("ruby_calls_js", proc { context.call("js_add", 20, 22) })
      raise "bad nested callback result" unless context.eval("ruby_calls_js()") == 42
    RUBY
  end

  def test_recursive_javascript_ruby_callback_ping_pong
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      context.attach("ruby_recurse", proc { |n|
        n <= 0 ? "done" : context.call("js_recurse", n - 1)
      })
      context.eval(<<~JS)
        function js_recurse(n) {
          if (n <= 0) return "done";
          return ruby_recurse(n);
        }
      JS
      raise "bad recursive callback result" unless context.call("js_recurse", 10) == "done"
    RUBY
  end

  def test_ruby_callback_exception_propagates
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      context.attach("boom", proc { raise "ruby boom" })

      begin
        context.eval("boom()")
        raise "expected callback exception"
      rescue RuntimeError => e
        raise "wrong exception: #{e.class}: #{e.message}" unless e.message.include?("ruby boom")
      end
    RUBY
  end

  def test_dispose_after_runner_started
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      raise "bad eval" unless context.eval("1 + 1") == 2
      context.dispose

      begin
        context.eval("1 + 1")
        raise "expected disposed error"
      rescue MiniRacerCsim::ContextDisposedError
      end

      context = nil
      GC.start
    RUBY
  end

  def test_multiple_contexts_and_dispose_one
    assert_single_threaded_script <<~'RUBY'
      a = MiniRacerCsim::Context.new
      b = MiniRacerCsim::Context.new

      a.eval("var x = 1")
      b.eval("var x = 2")

      raise "bad context a" unless a.eval("x") == 1
      raise "bad context b" unless b.eval("x") == 2

      a.dispose
      raise "context b broke after disposing a" unless b.eval("x + 40") == 42
    RUBY
  end

  def test_fork_after_runner_started_and_idle
    assert_single_threaded_script <<~'RUBY'
      exit 0 unless Process.respond_to?(:fork)

      context = MiniRacerCsim::Context.new
      context.eval("var answer = 41")
      context.eval("answer += 1") # starts the reusable runner and leaves it idle

      pid = fork do
        exit(context.eval("answer") == 42 ? 0 : 1)
      end
      Process.wait(pid)
      raise "child failed" unless $?.success?
    RUBY
  end

  def test_reset_realm
    # reset_realm swaps the realm by re-deriving the per-request Locals from the
    # persistents; in single-threaded mode that path runs on the shared Ruby
    # thread, so a regression that mishandles the Locker/scope would deadlock
    # (caught by the bounded wait) rather than just fail.
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      context.attach("host.add", proc { |a, b| a + b })
      context.eval("globalThis.leaked = 1")
      raise "bad host" unless context.eval("host.add(2, 3)") == 5

      context.reset_realm

      raise "global survived reset" unless context.eval("typeof globalThis.leaked") == "undefined"
      raise "host not re-attached" unless context.eval("host.add(20, 22)") == 42
      raise "fresh eval broke" unless context.eval("1 + 1") == 2
    RUBY
  end

  def test_load_module_graph
    # The batched fetch/resolve round-trips run on the shared Ruby thread in
    # single-threaded mode; a regression that mishandled the rendezvous would
    # deadlock (caught by the bounded wait) rather than just fail.
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      sources = {
        "/app.js" => 'import {a} from "./a.js"; globalThis.OUT = a + 5;',
        "/a.js"   => "export const a = 37;",
      }
      resolve = ->(edges) { edges.map { |spec, _ref| "/" + spec.sub(%r{\A\./}, "") } }
      fetch   = ->(urls)  { urls.map  { |u| (s = sources[u]) ? [s, nil] : nil } }

      r = context.load_module_graph("/app.js", resolve: resolve, fetch_batch: fetch)
      raise "bad eval" unless context.eval("globalThis.OUT") == 42
      raise "bad modules" unless r[:modules].map { |m| m[:url] }.sort == ["/a.js", "/app.js"]
    RUBY
  end

  def test_load_module_graph_dynamic_import_identity
    # Dynamic import after a graph load does nested resolve/fetch round-trips on
    # the shared Ruby thread; a rendezvous regression would deadlock here.
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new
      sources = {
        "/entry.js" => 'import {s} from "./shared.js"; s.n = 42; globalThis.OUT = "pending"; import("./shared.js").then(m => { globalThis.OUT = m.s.n; });',
        "/shared.js" => "export const s = { n: 0 };",
      }
      resolve = ->(edges) { edges.map { |spec, _ref| "/" + spec.sub(%r{\A\./}, "") } }
      fetch   = ->(urls)  { urls.map  { |u| (src = sources[u]) ? [src, nil] : nil } }
      context.load_module_graph("/entry.js", resolve: resolve, fetch_batch: fetch)
      raise "dynamic import did not reuse the loaded module" unless context.eval("globalThis.OUT") == 42
    RUBY
  end

  def test_host_namespace_drain_microtasks
    # The native checkpoint runs inline on the isolate thread and must not take
    # a v8::Locker, which would deadlock when V8 shares the Ruby thread.
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacerCsim::Context.new(host_namespace: "MiniRacer")
      order = context.eval(<<~JS)
        const seen = [];
        Promise.resolve().then(() => seen.push("microtask-fired"));
        seen.push("before-drain");
        MiniRacer.drainMicrotasks();
        seen.push("after-drain");
        seen;
      JS
      raise "bad drain order: #{order.inspect}" unless order == %w[before-drain microtask-fired after-drain]
    RUBY
  end

  def test_snapshot_with_realms_does_not_use_freed_startup_data
    # In single_threaded mode v8_thread_init returns while the isolate lives on,
    # destroying its stack frame. If params.snapshot_blob pointed at a stack
    # local, the post-boot Context::New inside create_realm / reset_realm would
    # dereference freed memory (SEGV, a V8 CHECK, or a silently corrupt realm).
    # The blob now lives in State, so a snapshotted context can spin up realms.
    assert_single_threaded_script <<~'RUBY'
      snapshot = MiniRacerCsim::Snapshot.new("function snap_fn() { return 7 }")
      ctx = MiniRacerCsim::Context.new(snapshot: snapshot)
      raise "snapshot not applied" unless ctx.eval("snap_fn()") == 7

      realm = ctx.create_realm
      raise "bad realm eval" unless realm.eval("1 + 2") == 3
      raise "snapshot fn missing in realm" unless realm.eval("snap_fn()") == 7

      ctx.reset_realm
      raise "bad post-reset eval" unless ctx.eval("3 + 4") == 7
      raise "snapshot fn missing after reset" unless ctx.eval("snap_fn()") == 7
    RUBY
  end
end
