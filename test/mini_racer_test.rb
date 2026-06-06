# frozen_string_literal: true

require "securerandom"
require "date"
require "test_helper"

class MiniRacerTest < Minitest::Test
  # see `test_platform_set_flags_works` below
  MiniRacerCsim::Platform.set_flags! :use_strict

  # --stress_snapshot works around a bogus debug assert in V8
  # that terminates the process with the following error:
  #
  # Fatal error in ../deps/v8/src/heap/read-only-spaces.cc, line 70
  # Check failed: read_only_blob_checksum_ == snapshot_checksum (<unprintable> vs. 1099685679).
  MiniRacerCsim::Platform.set_flags! :stress_snapshot

  def test_locale_mx
    val =
      MiniRacerCsim::Context.new.eval(
        "new Date('April 28 2021').toLocaleDateString('es-MX');"
      )
    assert_equal "28/4/2021", val
  end

  def test_locale_us
    val =
      MiniRacerCsim::Context.new.eval(
        "new Date('April 28 2021').toLocaleDateString('en-US');"
      )
    assert_equal "4/28/2021", val
  end

  def test_locale_fr
    # TODO: this causes a segfault on Linux

    val =
      MiniRacerCsim::Context.new.eval(
        "new Date('April 28 2021').toLocaleDateString('fr-FR');"
      )
    assert_equal "28/04/2021", val
  end

  def test_segfault
    skip "running this test is very slow"
    # 5000.times do
    #   GC.start
    #   context = MiniRacerCsim::Context.new(timeout: 5)
    #   context.attach("echo", proc{|msg| msg.to_sym.to_s})
    #   assert_raises(MiniRacerCsim::EvalError) do
    #     context.eval("while(true) echo('foo');")
    #   end
    # end
  end

  def test_that_it_has_a_version_number
    refute_nil ::MiniRacerCsim::VERSION
  end

  def test_types
    context = MiniRacerCsim::Context.new
    assert_equal 2, context.eval("2")
    assert_equal "two", context.eval('"two"')
    assert_equal 2.1, context.eval("2.1")
    assert_equal true, context.eval("true")
    assert_equal false, context.eval("false")
    assert_nil context.eval("null")
    assert_nil context.eval("undefined")
  end

  def test_compile_nil_context
    context = MiniRacerCsim::Context.new
    assert_raises(TypeError) { assert_equal 2, context.eval(nil) }
  end

  def test_array
    context = MiniRacerCsim::Context.new
    assert_equal [1, "two"], context.eval('[1,"two"]')
  end

  def test_object
    context = MiniRacerCsim::Context.new
    # remember JavaScript is quirky {"1" : 1} magically turns to {1: 1} cause magic
    assert_equal(
      { "1" => 2, "two" => "two" },
      context.eval('var a={"1" : 2, "two" : "two"}; a')
    )
  end

  def test_it_returns_runtime_error
    context = MiniRacerCsim::Context.new
    exp = nil

    begin
      context.eval("var foo=function(){boom;}; foo()")
    rescue => e
      exp = e
    end

    assert_equal MiniRacerCsim::RuntimeError, exp.class

    assert_match(/boom/, exp.message)
    assert_match(/foo/, exp.backtrace[0])
    assert_match(/mini_racer/, exp.backtrace[2])

    # context should not be dead
    assert_equal 2, context.eval("1+1")
  end

  def test_it_can_stop
    context = MiniRacerCsim::Context.new
    exp = nil

    begin
      Thread.new do
        sleep 0.01
        context.stop
      end
      context.eval("while(true){}")
    rescue => e
      exp = e
    end

    assert_equal MiniRacerCsim::ScriptTerminatedError, exp.class
    assert_match(/terminated/, exp.message)
  end

  def test_it_can_timeout_during_serialization
    context = MiniRacerCsim::Context.new(timeout: 500)

    assert_raises(MiniRacerCsim::ScriptTerminatedError) do
      context.eval "var a = {get a(){ while(true); }}; a"
    end
  end

  def test_it_can_automatically_time_out_context
    # 2 millisecs is a very short timeout but we don't want test running forever
    context = MiniRacerCsim::Context.new(timeout: 2)
    assert_raises { context.eval("while(true){}") }
  end

  def test_returns_javascript_function
    context = MiniRacerCsim::Context.new
    assert_same MiniRacerCsim::JavaScriptFunction,
                context.eval("var a = function(){}; a").class
  end

  def test_it_handles_malformed_js
    context = MiniRacerCsim::Context.new
    assert_raises MiniRacerCsim::ParseError do
      context.eval("I am not JavaScript {")
    end
  end

  def test_it_handles_malformed_js_with_backtrace
    context = MiniRacerCsim::Context.new
    assert_raises MiniRacerCsim::ParseError do
      begin
        context.eval("var i;\ni=2;\nI am not JavaScript {")
      rescue => e
        # I <parse error> am not
        assert_match(/3:2/, e.message)
        raise
      end
    end
  end

  def test_it_remembers_stuff_in_context
    context = MiniRacerCsim::Context.new
    context.eval("var x = function(){return 22;}")
    assert_equal 22, context.eval("x()")
  end

  def test_can_attach_functions
    context = MiniRacerCsim::Context.new
    context.eval "var adder"
    context.attach("adder", proc { |a, b| a + b })
    assert_equal 3, context.eval("adder(1,2)")
  end

  def test_es6_arrow_functions
    context = MiniRacerCsim::Context.new
    assert_equal 42, context.eval("var adder=(x,y)=>x+y; adder(21,21);")
  end

  def test_concurrent_access
    context = MiniRacerCsim::Context.new
    context.eval("var counter=0; var plus=()=>counter++;")

    (1..10).map { Thread.new { context.eval("plus()") } }.each(&:join)

    assert_equal 10, context.eval("counter")
  end

  class FooError < StandardError
    def initialize(message)
      super(message)
    end
  end

  def test_attached_exceptions
    context = MiniRacerCsim::Context.new
    context.attach("adder", proc { raise FooError, "I like foos" })
    assert_raises do
      begin
        raise FooError, "I like foos"
        context.eval("adder()")
      rescue => e
        assert_equal FooError, e.class
        assert_match(/I like foos/, e.message)
        # TODO backtrace splicing so js frames are injected
        raise
      end
    end
  end

  def test_attached_on_object
    context = MiniRacerCsim::Context.new
    context.eval "var minion"
    context.attach("minion.speak", proc { "banana" })
    assert_equal "banana", context.eval("minion.speak()")
  end

  def test_attached_on_nested_object
    context = MiniRacerCsim::Context.new
    context.eval "var minion"
    context.attach("minion.kevin.speak", proc { "banana" })
    assert_equal "banana", context.eval("minion.kevin.speak()")
  end

  def test_return_arrays
    context = MiniRacerCsim::Context.new
    context.eval "var nose"
    context.attach("nose.type", proc { ["banana", ["nose"]] })
    assert_equal ["banana", ["nose"]], context.eval("nose.type()")
  end

  def test_return_hash
    context = MiniRacerCsim::Context.new
    context.attach(
      "test",
      proc { { :banana => :nose, "inner" => { 42 => 42 } } }
    )
    assert_equal(
      { "banana" => "nose", "inner" => { "42" => 42 } },
      context.eval("test()")
    )
  end

  def test_date_nan
    # NoMethodError: undefined method `source_location' for "<internal:core>
    # core/float.rb:114:in `to_i'":Thread::Backtrace::Location
    context = MiniRacerCsim::Context.new
    assert_raises(RangeError) { context.eval("new Date(NaN)") } # should not crash process
  end

  def test_return_date
    context = MiniRacerCsim::Context.new
    test_time = Time.new
    test_datetime = test_time.to_datetime
    context.attach("test", proc { test_time })
    context.attach("test_datetime", proc { test_datetime })

    # check that marshalling to JS creates a date object (getTime())
    assert_equal(
      (test_time.to_f * 1000).to_i,
      context.eval("var result = test(); result.getTime();").to_i
    )

    # check that marshalling to RB creates a Time object
    result = context.eval("test()")
    assert_equal(test_time.class, result.class)
    assert_equal(test_time.tv_sec, result.tv_sec)

    # check that no precision is lost in the marshalling (js only stores milliseconds)
    assert_equal(
      (test_time.tv_usec / 1000.0).floor,
      (result.tv_usec / 1000.0).floor
    )

    # check that DateTime gets marshalled to js date and back out as rb Time
    result = context.eval("test_datetime()")
    assert_equal(test_time.class, result.class)
    assert_equal(test_time.tv_sec, result.tv_sec)
    assert_equal(
      (test_time.tv_usec / 1000.0).floor,
      (result.tv_usec / 1000.0).floor
    )
  end

  def test_datetime_missing
    # NoMethodError: undefined method `source_location' for
    # #<Thread::Backtrace::Location:0x4e88>
    date_time_backup = Object.send(:remove_const, :DateTime)

    begin
      # no exceptions should happen here, and non-datetime classes should marshall correctly still.
      context = MiniRacerCsim::Context.new
      test_time = Time.new
      context.attach("test", proc { test_time })

      assert_equal(
        (test_time.to_f * 1000).to_i,
        context.eval("var result = test(); result.getTime();").to_i
      )

      result = context.eval("test()")
      assert_equal(test_time.class, result.class)
      assert_equal(test_time.tv_sec, result.tv_sec)
      assert_equal(
        (test_time.tv_usec / 1000.0).floor,
        (result.tv_usec / 1000.0).floor
      )
    ensure
      Object.const_set(:DateTime, date_time_backup)
    end
  end

  def test_return_large_number
    context = MiniRacerCsim::Context.new
    test_num = 1_000_000_000_000_000
    context.attach("test", proc { test_num })

    assert_equal(true, context.eval("test() === 1000000000000000"))
    assert_equal(test_num, context.eval("test()"))
  end

  def test_return_int_max
    context = MiniRacerCsim::Context.new
    test_num = 2**(31) - 1 #last int32 number
    context.attach("test", proc { test_num })

    assert_equal(true, context.eval("test() === 2147483647"))
    assert_equal(test_num, context.eval("test()"))
  end

  def test_return_unknown
    context = MiniRacerCsim::Context.new
    test_unknown = Date.new # hits T_DATA in convert_ruby_to_v8
    context.attach("test", proc { test_unknown })
    assert_equal("Undefined Conversion", context.eval("test()"))

    # clean up and start up a new context
    context = nil
    GC.start

    context = MiniRacerCsim::Context.new
    test_unknown = Date.new # hits T_DATA in convert_ruby_to_v8
    context.attach("test", proc { test_unknown })
    assert_equal("Undefined Conversion", context.eval("test()"))
  end

  def test_max_memory
    context = MiniRacerCsim::Context.new(max_memory: 200_000_000)

    assert_raises(MiniRacerCsim::V8OutOfMemoryError) do
      context.eval(
        "let s = 1000; var a = new Array(s); a.fill(0); while(true) {s *= 1.1; let n = new Array(Math.floor(s)); n.fill(0); a = a.concat(n); };"
      )
    end
  end

  def test_max_memory_for_call
    context = MiniRacerCsim::Context.new(max_memory: 100_000_000)
    context.eval(<<~JS)
      let s;
      function memory_test() {
        var a = new Array(s);
        a.fill(0);
        while(true) {
          s *= 1.1;
          let n = new Array(Math.floor(s));
          n.fill(0);
          a = a.concat(n);
          if (s > 1000000) {
            return;
          }
        }
      }
      function set_s(val) {
        s = val;
      }
    JS
    context.call("set_s", 1000)
    assert_raises(MiniRacerCsim::V8OutOfMemoryError) { context.call("memory_test") }
    s = context.eval("s")
    assert_operator(s, :>, 100_000)
  end

  def test_max_memory_bounds
    assert_raises(ArgumentError) do
      MiniRacerCsim::Context.new(max_memory: -200_000_000)
    end

    assert_raises(ArgumentError) { MiniRacerCsim::Context.new(max_memory: 2**32) }
  end

  module Echo
    def self.say(thing)
      thing
    end
  end

  def test_can_attach_method
    context = MiniRacerCsim::Context.new
    context.eval "var Echo"
    context.attach("Echo.say", Echo.method(:say))
    assert_equal "hello", context.eval("Echo.say('hello')")
  end

  def test_attach_non_object
    context = MiniRacerCsim::Context.new
    context.eval("var minion = 2")
    context.attach("minion.kevin.speak", proc { "banana" })
    assert_equal "banana", context.call("minion.kevin.speak")
  end

  def test_load
    context = MiniRacerCsim::Context.new
    context.load(File.dirname(__FILE__) + "/file.js")
    assert_equal "world", context.eval("hello")
    assert_raises { context.load(File.dirname(__FILE__) + "/missing.js") }
  end

  def test_contexts_can_be_safely_GCed
    context = MiniRacerCsim::Context.new
    context.eval 'var hello = "world";'

    context = nil
    GC.start
  end

  def test_it_can_use_snapshots
    snapshot =
      MiniRacerCsim::Snapshot.new(
        'function hello() { return "world"; }; var foo = "bar";'
      )

    context = MiniRacerCsim::Context.new(snapshot: snapshot)

    assert_equal "world", context.eval("hello()")
    assert_equal "bar", context.eval("foo")
  end

  def test_snapshot_size
    snapshot = MiniRacerCsim::Snapshot.new('var foo = "bar";')

    # for some reason sizes seem to change across runs, so we just
    # check it's a positive integer
    assert(snapshot.size > 0)
  end

  def test_snapshot_dump
    snapshot = MiniRacerCsim::Snapshot.new('var foo = "bar";')
    dump = snapshot.dump

    assert_equal(String, dump.class)
    assert_equal(Encoding::ASCII_8BIT, dump.encoding)
    assert_equal(snapshot.size, dump.length)
  end

  def test_snapshot_load
    snapshot = MiniRacerCsim::Snapshot.new('var foo = "bar"; function hello() { return "world"; }')
    blob = snapshot.dump

    restored = MiniRacerCsim::Snapshot.load(blob)

    assert_equal(snapshot.size, restored.size)
    assert_equal(Encoding::ASCII_8BIT, restored.dump.encoding)
    assert(restored.dump.valid_encoding?, "restored snapshot dump should have valid encoding")
    ctx = MiniRacerCsim::Context.new(snapshot: restored)
    assert_equal("bar", ctx.eval("foo"))
    assert_equal("world", ctx.eval("hello()"))
  end

  def test_snapshot_load_with_non_binary_encoding
    snapshot = MiniRacerCsim::Snapshot.new('var foo = "bar";')
    # Force non-binary encoding to exercise the coderange fix.
    # Binary data interpreted as UTF-8 will have broken encoding.
    blob = snapshot.dump.dup.force_encoding("UTF-8")
    assert_equal(Encoding::UTF_8, blob.encoding)
    assert(!blob.valid_encoding?, "test precondition: blob should have broken UTF-8 encoding")

    restored = MiniRacerCsim::Snapshot.load(blob)

    assert_equal(Encoding::ASCII_8BIT, restored.dump.encoding)
    assert(restored.dump.valid_encoding?, "restored snapshot should have valid binary encoding")
    ctx = MiniRacerCsim::Context.new(snapshot: restored)
    assert_equal("bar", ctx.eval("foo"))
  end

  def test_invalid_snapshots_throw_an_exception
    begin
      MiniRacerCsim::Snapshot.new("var foo = bar;")
    rescue MiniRacerCsim::SnapshotError => e
      assert(e.backtrace[0].include? "JavaScript")
      got_error = true
    end

    assert(got_error, "should raise")
  end

  def test_an_empty_snapshot_is_valid
    MiniRacerCsim::Snapshot.new("")
    MiniRacerCsim::Snapshot.new
    GC.start
  end

  def test_snapshots_can_be_warmed_up_with_no_side_effects
    # shamelessly inspired by https://github.com/v8/v8/blob/5.3.254/test/cctest/test-serialize.cc#L792-L854
    snapshot_source = <<-JS
      function f() { return Math.sin(1); }
      var a = 5;
    JS

    snapshot = MiniRacerCsim::Snapshot.new(snapshot_source)

    warmup_source = <<-JS
      Math.tan(1);
      var a = f();
      Math.sin = 1;
    JS

    warmed_up_snapshot = snapshot.warmup!(warmup_source)

    context = MiniRacerCsim::Context.new(snapshot: snapshot)

    assert_equal 5, context.eval("a")
    assert_equal "function", context.eval("typeof(Math.sin)")
    assert_same snapshot, warmed_up_snapshot
  end

  def test_invalid_warmup_sources_throw_an_exception
    assert_raises(MiniRacerCsim::SnapshotError) do
      MiniRacerCsim::Snapshot.new("Math.sin = 1;").warmup!("var a = Math.sin(1);")
    end
  end

  def test_invalid_warmup_sources_throw_an_exception_2
    assert_raises(TypeError) do
      MiniRacerCsim::Snapshot.new("function f() { return 1 }").warmup!([])
    end
  end

  def test_warming_up_with_invalid_source_does_not_affect_the_snapshot_internal_state
    snapshot = MiniRacerCsim::Snapshot.new("Math.sin = 1;")

    begin
      snapshot.warmup!("var a = Math.sin(1);")
    rescue StandardError
      # do nothing
    end

    context = MiniRacerCsim::Context.new(snapshot: snapshot)

    assert_equal 1, context.eval("Math.sin")
  end

  def test_snapshots_can_be_GCed_without_affecting_contexts_created_from_them
    snapshot = MiniRacerCsim::Snapshot.new("Math.sin = 1;")
    context = MiniRacerCsim::Context.new(snapshot: snapshot)

    # force the snapshot to be GC'ed
    snapshot = nil
    GC.start

    # the context should still work fine
    assert_equal 1, context.eval("Math.sin")
  end

  def test_isolates_from_snapshot_dont_get_corrupted_if_the_snapshot_gets_warmed_up_or_GCed
    # basically tests that isolates get their own copy of the snapshot and don't
    # get corrupted if the snapshot is subsequently warmed up
    snapshot_source = <<-JS
      function f() { return Math.sin(1); }
      var a = 5;
    JS

    snapshot = MiniRacerCsim::Snapshot.new(snapshot_source)

    warmump_source = <<-JS
      Math.tan(1);
      var a = f();
      Math.sin = 1;
    JS

    snapshot.warmup!(warmump_source)

    context1 = MiniRacerCsim::Context.new(snapshot: snapshot)

    assert_equal 5, context1.eval("a")
    assert_equal "function", context1.eval("typeof(Math.sin)")

    GC.start

    context2 = MiniRacerCsim::Context.new(snapshot: snapshot)

    assert_equal 5, context2.eval("a")
    assert_equal "function", context2.eval("typeof(Math.sin)")
  end

  def test_platform_set_flags_raises_an_exception_if_already_initialized
    # makes sure it's initialized
    MiniRacerCsim::Snapshot.new

    assert_raises(MiniRacerCsim::PlatformAlreadyInitialized) do
      MiniRacerCsim::Platform.set_flags! :noconcurrent_recompilation
    end
  end

  def test_platform_set_flags_works
    context = MiniRacerCsim::Context.new

    assert_raises(MiniRacerCsim::RuntimeError) do
      # should fail because of strict mode set for all these tests
      context.eval "x = 28"
    end
  end

  def test_error_on_return_val
    v8 = MiniRacerCsim::Context.new
    assert_raises(MiniRacerCsim::RuntimeError) do
      v8.eval(
        'var o = {}; o.__defineGetter__("bar", function() { return null(); }); o'
      )
    end
  end

  def test_ruby_based_property_in_rval
    v8 = MiniRacerCsim::Context.new
    v8.attach "echo", proc { |x| x }
    assert_equal(
      { "bar" => 42 },
      v8.eval("var o = {get bar() { return echo(42); }}; o")
    )
  end

  def test_function_rval
    context = MiniRacerCsim::Context.new
    context.attach("echo", proc { |msg| msg })
    assert_equal("foo", context.eval("echo('foo')"))
  end

  def test_timeout_in_ruby_land
    skip "TODO(bnoordhuis) need to think on how to interrupt ruby code"
    context = MiniRacerCsim::Context.new(timeout: 50)
    context.attach("sleep", proc { sleep 10 })
    assert_raises(MiniRacerCsim::ScriptTerminatedError) do
      context.eval('sleep(); "hi";')
    end
  end

  def test_undef_mem
    context = MiniRacerCsim::Context.new(timeout: 5)

    context.attach(
      "marsh",
      proc do |a, b, c|
        a[rand(10_000).to_s] = "a"
        b[rand(10_000).to_s] = "b"
        c[rand(10_000).to_s] = "c"
        [a, b, c]
      end
    )

    assert_raises do
      # TODO make it raise the correct exception!
      context.eval(
        "var a = [{},{},{}]; while(true) { a = marsh(a[0],a[1],a[2]); }"
      )
    end
  end

  def test_can_dispose_context
    context = MiniRacerCsim::Context.new(timeout: 5)
    context.dispose
    assert_raises(MiniRacerCsim::ContextDisposedError) { context.eval("a") }
  end

  def test_estimated_size
    context = MiniRacerCsim::Context.new(timeout: 500)
    context.eval(<<~JS)
      let a='testing';
      let f=function(foo) { foo + 42 };

      // call `f` a lot to have things JIT'd so that total_heap_size_executable becomes > 0
      for (let i = 0; i < 1000000; i++) { f(10); }
    JS

    stats = context.heap_stats
    # eg: {:total_physical_size=>1280640, :total_heap_size_executable=>4194304, :total_heap_size=>3100672, :used_heap_size=>1205376, :heap_size_limit=>1501560832}
    assert_equal(
      %i[
        external_memory
        heap_size_limit
        malloced_memory
        number_of_detached_contexts
        number_of_native_contexts
        peak_malloced_memory
        total_available_size
        total_global_handles_size
        total_heap_size
        total_heap_size_executable
        total_physical_size
        used_global_handles_size
        used_heap_size
      ].sort,
      stats.keys.sort
    )

    assert_equal 0, stats[:external_memory]
    assert_equal 0, stats[:number_of_detached_contexts]
    stats.delete :external_memory
    stats.delete :number_of_detached_contexts

    assert(
      stats.values.all? { |v| v > 0 },
      "expecting the isolate to have values for all the vals: actual stats #{stats}"
    )
  end

  def test_releasing_memory
    context = MiniRacerCsim::Context.new

    context.low_memory_notification

    start_heap = context.heap_stats[:used_heap_size]

    context.eval("'#{"x" * 1_000_000}'")

    context.low_memory_notification

    end_heap = context.heap_stats[:used_heap_size]

    assert(
      (end_heap - start_heap).abs < 1000,
      "expecting most of the 1_000_000 long string to be freed"
    )
  end

  def test_bad_params
    assert_raises { MiniRacerCsim::Context.new(random: :thing) }
  end

  def test_ensure_gc
    context = MiniRacerCsim::Context.new(ensure_gc_after_idle: 1)
    context.low_memory_notification

    start_heap = context.heap_stats[:used_heap_size]

    context.eval("'#{"x" * 10_000_000}'")

    sleep 0.01

    end_heap = context.heap_stats[:used_heap_size]

    assert(
      (end_heap - start_heap).abs < 1000,
      "expecting most of the 1_000_000 long string to be freed"
    )
  end

  def test_eval_with_filename
    context = MiniRacerCsim::Context.new()
    context.eval("var foo = function(){baz();}", filename: "b/c/foo1.js")

    got_error = false
    begin
      context.eval("foo()", filename: "baz1.js")
    rescue MiniRacerCsim::RuntimeError => e
      assert_match(/foo1.js/, e.backtrace[0])
      assert_match(/baz1.js/, e.backtrace[1])
      got_error = true
    end

    assert(got_error, "should raise")
  end

  def test_estimated_size_when_disposed
    context = MiniRacerCsim::Context.new(timeout: 50)
    context.eval("let a='testing';")
    context.dispose

    assert_raises(MiniRacerCsim::ContextDisposedError) { context.heap_stats }
  end

  def test_can_dispose
    skip "takes too long"
    #
    # junk_it_up
    # 3.times do
    #   GC.start(full_mark: true, immediate_sweep: true)
    # end
  end

  def junk_it_up
    1000.times do
      context = MiniRacerCsim::Context.new(timeout: 5)
      context.dispose
    end
  end

  def test_attached_recursion
    context = MiniRacerCsim::Context.new(timeout: 200)
    context.attach("a", proc { |a| a })
    context.attach("b", proc { |a| a })

    context.eval("const obj = {get r(){ b() }}; a(obj);")
  end

  def test_heap_dump
    f = Tempfile.new("heap")
    path = f.path
    f.unlink

    context = MiniRacerCsim::Context.new
    context.eval("let x = 1000;")
    context.write_heap_snapshot(path)

    dump = File.read(path)

    assert dump.length > 0

    FileUtils.rm(path)
  end

  def test_pipe_leak
    # in Ruby 2.7 pipes will stay open for longer
    # make sure that we clean up early so pipe file
    # descriptors are not kept around
    context = MiniRacerCsim::Context.new(timeout: 1000)
    10_000.times { |i| context.eval("'hello'") }
  end

  def test_symbol_support
    context = MiniRacerCsim::Context.new()
    assert_equal "foo", context.eval("Symbol('foo')")
    assert_nil context.eval("Symbol()") # should not crash
  end

  def test_infinite_object_js
    context = MiniRacerCsim::Context.new
    context.attach("a", proc { |a| a })

    js = <<~JS
      var d=0;
      function get(z) {
        z.depth=d++; // this isn't necessary to make it infinite, just to make it more obvious that it is
        Object.defineProperty(z,'foo',{get(){var r={};return get(r);},enumerable:true})
        return z;
      }
      a(get({}));
    JS

    assert_raises(MiniRacerCsim::RuntimeError) { context.eval(js) }
  end

  def test_deep_object_js
    context = MiniRacerCsim::Context.new
    context.attach("a", proc { |a| a })

    # stack depth should be enough to marshal the object
    assert_equal [[[]]], context.eval("let arr = [[[]]]; a(arr)")

    # too deep
    assert_raises(MiniRacerCsim::RuntimeError) do
      context.eval("let arr = [[[[[[[[]]]]]]]]; a(arr)")
    end
  end

  def test_wasm_ref
    context = MiniRacerCsim::Context.new
    expected = {}
    actual =
      context.eval(
        "
      var b = [0,97,115,109,1,0,0,0,1,26,5,80,0,95,0,80,0,95,1,127,0,96,0,1,110,96,1,100,2,1,111,96,0,1,100,3,3,4,3,3,2,4,7,26,2,12,99,114,101,97,116,101,83,116,114,117,99,116,0,1,7,114,101,102,70,117,110,99,0,2,9,5,1,3,0,1,0,10,23,3,8,0,32,0,20,2,251,27,11,7,0,65,12,251,0,1,11,4,0,210,0,11,0,44,4,110,97,109,101,1,37,3,0,11,101,120,112,111,114,116,101,100,65,110,121,1,12,99,114,101,97,116,101,83,116,114,117,99,116,2,7,114,101,102,70,117,110,99]
      var o = new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array(b))).exports
      o.refFunc()(o.createStruct) // exotic object
    "
      )
    assert_equal expected, actual
  end

  def test_proxy_support
    js = <<~JS
      function MyProxy(reference) {
        return new Proxy(function() {}, {
          get: function(obj, prop) {
            if (prop === Symbol.toPrimitive) return reference[prop];
            return new MyProxy(reference.concat(prop));
          },
          apply: function(target, thisArg, argumentsList) {
            myFunctionLogger(reference);
          }
        });
      };
      (new MyProxy([])).function_call(new MyProxy([])-1)
    JS
    context = MiniRacerCsim::Context.new()
    context.attach("myFunctionLogger", ->(property) {})
    context.eval(js)
  end

  def test_proxy_uncloneable
    context = MiniRacerCsim::Context.new()
    expected = { "x" => 42 }
    assert_equal expected, context.eval(<<~JS)
      const o = {x: 42}
      const p = new Proxy(o, {})
      Object.seal(p)
    JS
  end

  def test_promise
    context = MiniRacerCsim::Context.new()
    context.eval <<~JS
      var x = 0;
      async function test() {
        return 99;
      }

      test().then(v => x = v);
    JS

    v = context.eval("x")
    assert_equal(v, 99)
  end

  def test_perform_microtask_checkpoint_returns_nil
    context = MiniRacerCsim::Context.new
    assert_nil(context.perform_microtask_checkpoint)
  end

  def test_perform_microtask_checkpoint_drains_from_callback
    context = MiniRacerCsim::Context.new
    seen    = []

    context.attach('note',  ->(s) { seen << s })
    context.attach('drain', -> { context.perform_microtask_checkpoint })

    context.eval <<~JS
      Promise.resolve().then(() => note('microtask-fired'));
      note('before-drain');
      drain();
      note('after-drain');
    JS

    assert_equal(%w[before-drain microtask-fired after-drain], seen)
  end

  # Creates a context with the host namespace installed.
  def host_namespace_context(host_namespace: "MiniRacer", **options)
    MiniRacerCsim::Context.new(host_namespace: host_namespace, **options)
  end

  def test_host_namespace_not_installed_by_default
    context = MiniRacerCsim::Context.new

    # Opt-in only: without host_namespace nothing is injected. (drainMicrotasks
    # is the real method name; assert it never leaks as a bare global either.)
    assert_equal("undefined", context.eval("typeof MiniRacer"))
    assert_equal("undefined", context.eval("typeof drainMicrotasks"))
  end

  def test_host_namespace_false_or_empty_is_off
    assert_equal("undefined", MiniRacerCsim::Context.new(host_namespace: false).eval("typeof MiniRacer"))
    assert_equal("undefined", MiniRacerCsim::Context.new(host_namespace: "").eval("typeof MiniRacer"))
  end

  def test_host_namespace_rejects_invalid_type
    # A non-String/Symbol name is a type error (C Check_Type raises TypeError).
    assert_raises(TypeError, ArgumentError) do
      MiniRacerCsim::Context.new(host_namespace: 123)
    end
  end

  def test_host_namespace_rejects_invalid_identifier
    # Non-identifier names would only be reachable via globalThis["..."], not as
    # `<name>.drainMicrotasks()`, so they are rejected up front.
    ["foo-bar", "foo.bar", "123abc", "with space", "a/b"].each do |name|
      assert_raises(ArgumentError) do
        MiniRacerCsim::Context.new(host_namespace: name)
      end
    end
  end

  def test_host_namespace_allows_identifier_punctuation
    context = host_namespace_context(host_namespace: "$mr_2")
    assert_equal("function", context.eval("typeof $mr_2.drainMicrotasks"))
  end

  def test_host_namespace_true_uses_default_name
    context = host_namespace_context(host_namespace: true)
    assert_equal("function", context.eval("typeof MiniRacer.drainMicrotasks"))
  end

  def test_host_namespace_accepts_a_custom_name
    context = host_namespace_context(host_namespace: "App")

    assert_equal("function", context.eval("typeof App.drainMicrotasks"))
    assert_equal("undefined", context.eval("typeof MiniRacer"))
  end

  def test_host_namespace_is_non_enumerable_but_methods_are_discoverable
    context = host_namespace_context

    # The namespace object itself stays out of globalThis enumeration...
    refute(context.eval("Object.keys(globalThis).includes('MiniRacer')"))
    refute(context.eval("Object.getOwnPropertyDescriptor(globalThis, 'MiniRacer').enumerable"))
    # ...but its methods are ordinary own properties, so they are discoverable.
    assert_equal(%w[drainMicrotasks], context.eval("Object.keys(MiniRacer)"))
  end

  def test_host_namespace_drain_microtasks_inline
    context = host_namespace_context

    # Unlike perform_microtask_checkpoint, no Ruby callback round-trip is
    # needed: JS drains the queue mid-execution by calling the native method.
    order = context.eval(<<~JS)
      const seen = [];
      Promise.resolve().then(() => seen.push("microtask-fired"));
      seen.push("before-drain");
      MiniRacer.drainMicrotasks();
      seen.push("after-drain");
      seen;
    JS

    assert_equal(%w[before-drain microtask-fired after-drain], order)
  end

  def test_host_namespace_drain_microtasks_is_a_noop_when_nested
    context = host_namespace_context

    # Calling it from inside a running microtask (depth > 0) must be a guarded
    # no-op that does not synchronously re-enter the queue. m1 enqueues m3 then
    # drains; under a correct no-op m3 runs only after m1 returns, so "m1-end"
    # precedes "m3". A re-entering (force-nesting) implementation would run m3
    # inside m1 and yield "m3" before "m1-end".
    order = context.eval(<<~JS)
      const seen = [];
      Promise.resolve().then(() => {
        seen.push("m1-start");
        Promise.resolve().then(() => seen.push("m3"));
        MiniRacer.drainMicrotasks();
        seen.push("m1-end");
      });
      MiniRacer.drainMicrotasks();
      seen;
    JS

    assert_equal(%w[m1-start m1-end m3], order)
  end

  def test_host_namespace_drain_microtasks_does_not_propagate_microtask_exceptions
    context = host_namespace_context

    # An exception thrown by a drained microtask is routed to V8's handlers
    # (as with perform_microtask_checkpoint), not propagated to the caller: the
    # drain returns normally and the context stays usable.
    order = context.eval(<<~JS)
      const seen = [];
      Promise.resolve().then(() => { throw new Error("boom"); });
      seen.push("before");
      MiniRacer.drainMicrotasks();
      seen.push("after");
      seen;
    JS

    assert_equal(%w[before after], order)
    assert_equal(2, context.eval("1 + 1"))
  end

  def test_host_namespace_drain_microtasks_returns_undefined
    context = host_namespace_context
    assert_nil(context.eval("MiniRacer.drainMicrotasks()"))
  end

  def test_host_namespace_installed_on_snapshot_backed_context
    snapshot = MiniRacerCsim::Snapshot.new("var fromSnapshot = 42;")
    context = host_namespace_context(snapshot: snapshot)

    # The namespace closes over native pointers and is not part of the
    # snapshot; it must be (re)installed on every fresh context.
    assert_equal("function", context.eval("typeof MiniRacer.drainMicrotasks"))
    assert_equal(42, context.eval("fromSnapshot"))
  end

  def test_host_namespace_drain_microtasks_surfaces_termination
    context = host_namespace_context(timeout: 50)

    # A runaway microtask drained here must let watchdog termination propagate
    # to the enclosing eval. Asserting that the trailing statement did not run
    # (rather than only that an error was raised) proves the termination came
    # from the inline drain: were drainMicrotasks() a no-op, the kAuto
    # checkpoint at script end would still terminate, but reached_after would
    # already be true.
    assert_raises(MiniRacerCsim::ScriptTerminatedError) do
      context.eval(<<~JS)
        globalThis.reached_after = false;
        Promise.resolve().then(() => { while (true) {} });
        MiniRacer.drainMicrotasks();
        globalThis.reached_after = true;
      JS
    end

    refute(context.eval("globalThis.reached_after"))
  end

  def test_host_namespace_drain_microtasks_surfaces_out_of_memory
    context = host_namespace_context(max_memory: 100_000_000)

    # Same as above for the out-of-memory path (v8_gc_callback), which the
    # README documents alongside watchdog termination.
    assert_raises(MiniRacerCsim::V8OutOfMemoryError) do
      context.eval(<<~JS)
        globalThis.reached_after = false;
        Promise.resolve().then(() => {
          let s = 1000, a = new Array(s); a.fill(0);
          while (true) { s *= 1.1; let n = new Array(Math.floor(s)); n.fill(0); a = a.concat(n); }
        });
        MiniRacer.drainMicrotasks();
        globalThis.reached_after = true;
      JS
    end

    refute(context.eval("globalThis.reached_after"))
  end

  def reset_realm_context(**options)
    MiniRacerCsim::Context.new(**options)
  end

  def test_reset_realm_clears_globals_but_keeps_eval_working
    context = reset_realm_context

    context.eval("globalThis.leaked = 42")
    assert_equal(42, context.eval("globalThis.leaked"))

    context.reset_realm

    # The whole realm (globalThis and everything on it) is gone...
    assert_equal("undefined", context.eval("typeof globalThis.leaked"))
    # ...but the context is still usable against the fresh realm.
    assert_equal(2, context.eval("1 + 1"))
  end

  def test_reset_realm_reattaches_host_functions
    context = reset_realm_context

    calls = 0
    # Two functions at different dotted paths, exercising the re-bind loop over
    # every registered callback (not just the last one).
    context.attach("host.add", proc { |a, b| calls += 1; a + b })
    context.attach("util.mul", proc { |a, b| a * b })
    assert_equal(5, context.eval("host.add(2, 3)"))

    context.reset_realm

    # The JS shims were dropped with the old global; reset_realm re-binds them
    # onto the fresh global so the same Ruby procs keep working. Check both the
    # eval path and the call() name-resolution path.
    assert_equal(30, context.eval("host.add(10, 20)"))
    assert_equal(30, context.call("host.add", 10, 20))
    assert_equal(42, context.call("util.mul", 6, 7))
    assert_equal(3, calls)
  end

  def test_reset_realm_is_refused_from_within_a_host_callback
    context = reset_realm_context

    # Resetting the realm while a JS->Ruby callback is suspended mid-roundtrip
    # would swap the realm out from under the callback frame; it must be refused.
    context.attach("boom", proc { context.reset_realm })
    error = assert_raises(MiniRacerCsim::RuntimeError) { context.eval("boom()") }
    assert_match(/within a host function callback/, error.message)

    # The context must remain healthy after the refusal.
    assert_equal(2, context.eval("1 + 1"))
  end

  def test_reset_realm_reinstalls_host_namespace
    context = reset_realm_context(host_namespace: "MiniRacer")

    assert_equal("function", context.eval("typeof MiniRacer.drainMicrotasks"))
    context.reset_realm
    assert_equal("function", context.eval("typeof MiniRacer.drainMicrotasks"))
  end

  def test_reset_realm_restores_snapshot_globals
    snapshot = MiniRacerCsim::Snapshot.new("globalThis.VENDOR = { ok: 7 };")
    context = reset_realm_context(snapshot: snapshot)

    assert_equal(7, context.eval("VENDOR.ok"))
    context.eval("VENDOR.ok = 999; globalThis.perPage = true")
    context.reset_realm

    # The fresh realm comes from the same startup snapshot, so vendor globals
    # are back at their pristine values and per-page mutations are gone.
    assert_equal(7, context.eval("VENDOR.ok"))
    assert_equal("undefined", context.eval("typeof globalThis.perPage"))
  end

  def test_reset_realm_is_repeatable
    context = reset_realm_context

    5.times do |i|
      context.eval("globalThis.n = #{i}")
      assert_equal(i, context.eval("globalThis.n"))
      context.reset_realm
      assert_equal("undefined", context.eval("typeof globalThis.n"))
    end
  end

  def test_reset_realm_drops_pending_unhandled_rejections
    context = reset_realm_context

    fired = []
    context.attach("record", proc { |reason| fired << reason })

    # Reject a promise with no handler but do NOT drain: the plain eval path
    # queues it for the next checkpoint without delivering it (only
    # drainMicrotasks / perform_microtask_checkpoint / pump_message_loop notify).
    context.eval("Promise.reject(new Error('stale'));")

    # reset_realm rebuilds the main realm (reusing id 0). The queued rejection
    # belongs to the OLD realm 0; it must be dropped, not fired against the
    # fresh realm 0 (whose globalThis is a different object).
    context.reset_realm

    context.eval("globalThis.__mr_emitUnhandledRejection = (reason) => record(String(reason));")
    context.perform_microtask_checkpoint

    assert_empty(fired, "a pre-reset rejection must not fire in the fresh realm")
  end

  def test_reset_realm_collects_the_old_realm
    context = reset_realm_context

    bloat = 'globalThis.junk = Array.from({length: 200000}, (_, i) => ({ i, s: "x".repeat(20) }));'
    context.eval(bloat)
    context.low_memory_notification
    baseline = context.heap_stats[:used_heap_size]

    20.times do
      context.eval(bloat)
      context.reset_realm
    end
    context.low_memory_notification
    final = context.heap_stats[:used_heap_size]

    # Each iteration leaks ~24MB of realm-scoped state if the old realm is not
    # collected. If reset_realm works, the heap returns to roughly baseline
    # regardless of how many resets ran.
    assert_operator(final, :<, baseline * 2)
  end

  # -- per-frame realms (Context#create_realm / MiniRacerCsim::Realm) --

  def test_create_realm_returns_a_realm
    ctx = MiniRacerCsim::Context.new
    realm = ctx.create_realm
    assert_kind_of MiniRacerCsim::Realm, realm
    assert_kind_of Integer, realm.id
    assert_equal 3, realm.eval("1 + 2")
  end

  def test_realm_global_is_isolated_from_the_main_realm
    ctx = MiniRacerCsim::Context.new
    ctx.eval("globalThis.x = 1")
    realm = ctx.create_realm
    assert_equal "undefined", realm.eval("typeof globalThis.x")
    realm.eval("globalThis.x = 99")
    assert_equal 1, ctx.eval("globalThis.x")  # main realm unchanged
    assert_equal 99, realm.eval("globalThis.x")
  end

  def test_realm_inherits_attached_host_functions
    ctx = MiniRacerCsim::Context.new
    ctx.attach("hostAdd", ->(a, b) { a + b })
    realm = ctx.create_realm
    assert_equal 5, realm.eval("hostAdd(2, 3)")
  end

  def test_realm_call_and_attach
    ctx = MiniRacerCsim::Context.new
    realm = ctx.create_realm
    realm.eval("globalThis.f = (a) => a * 10")
    assert_equal 40, realm.call("f", 4)
    realm.attach("hostMul", ->(a, b) { a * b })
    assert_equal 12, realm.eval("hostMul(3, 4)")
  end

  def test_multiple_realms_are_independent
    ctx = MiniRacerCsim::Context.new
    a = ctx.create_realm
    b = ctx.create_realm
    refute_equal a.id, b.id
    a.eval("globalThis.v = 'a'")
    b.eval("globalThis.v = 'b'")
    assert_equal "a", a.eval("globalThis.v")
    assert_equal "b", b.eval("globalThis.v")
  end

  def test_realm_dispose
    ctx = MiniRacerCsim::Context.new
    realm = ctx.create_realm
    refute realm.disposed?
    realm.dispose
    assert realm.disposed?
    assert_raises(MiniRacerCsim::Error) { realm.eval("1") }
    realm.dispose # idempotent
  end

  def test_realm_global_is_live_and_shared_cross_realm
    ctx = MiniRacerCsim::Context.new
    a = ctx.create_realm
    b = ctx.create_realm
    b.eval("globalThis.shared = { n: 42 }")
    a.eval("globalThis.B = #{b.id}")
    # A reads B's global as a live object (not a copy)...
    assert_equal 42, a.eval("__mr_realmGlobal(B).shared.n")
    # ...and a mutation from A is visible in B = same underlying object.
    a.eval("__mr_realmGlobal(B).shared.n = 100")
    assert_equal 100, b.eval("globalThis.shared.n")
  end

  def test_realm_global_cross_realm_function_call
    ctx = MiniRacerCsim::Context.new
    a = ctx.create_realm
    b = ctx.create_realm
    b.eval("globalThis.inc = (x) => x + 1")
    a.eval("globalThis.B = #{b.id}")
    assert_equal 6, a.eval("__mr_realmGlobal(B).inc(5)")
  end

  def test_realm_global_reaches_main_realm
    ctx = MiniRacerCsim::Context.new
    ctx.eval("globalThis.m = 7")
    a = ctx.create_realm
    assert_equal 7, a.eval("__mr_realmGlobal(0).m")
  end

  def test_realm_global_unknown_realm_is_undefined
    ctx = MiniRacerCsim::Context.new
    a = ctx.create_realm
    assert_equal "undefined", a.eval("typeof __mr_realmGlobal(99999)")
  end

  def test_realm_has_independent_intrinsics
    ctx = MiniRacerCsim::Context.new
    a = ctx.create_realm
    b = ctx.create_realm
    a.eval("globalThis.B = #{b.id}")
    # Each realm has its own intrinsics (like an iframe): B's Object is not A's.
    assert_equal false, a.eval("__mr_realmGlobal(B).Object === Object")
    # An object built by B is `instanceof` B's Object, but not A's.
    b.eval("globalThis.o = {}")
    assert_equal true, a.eval("__mr_realmGlobal(B).o instanceof __mr_realmGlobal(B).Object")
    assert_equal false, a.eval("__mr_realmGlobal(B).o instanceof Object")
  end

  def test_create_realm_is_reentrant_from_a_host_function
    ctx = MiniRacerCsim::Context.new
    made = nil
    # Lazy model: a host function (called mid-eval) creates a realm. This must
    # not corrupt the suspended outer frame (it used to SEGV via a dangling
    # active-realm handle).
    ctx.attach("makeRealm", -> { made = ctx.create_realm; made.id })
    id = ctx.eval("makeRealm()")
    assert_kind_of Integer, id
    assert_equal id, made.id
    # the new realm works...
    made.eval("globalThis.z = 5")
    assert_equal 5, made.eval("globalThis.z")
    # ...and the outer realm survived the re-entrant creation.
    assert_equal 2, ctx.eval("1 + 1")
  end

  def test_realm_eval_is_reentrant_with_continuing_outer_eval
    ctx = MiniRacerCsim::Context.new
    # A host function (mid-eval) creates a realm and evals in it, then the outer
    # eval *continues*. The re-entrant Realm#eval must restore the caller's V8
    # context or the resuming outer frame SEGVs (sibling of the create_realm bug).
    ctx.attach("mk", lambda {
      r = ctx.create_realm
      r.eval("globalThis.x = 1")
      r.id
    })
    assert_equal "after 1", ctx.eval('var id = mk(); "after " + id')
  end

  def test_realm_reentrant_then_outer_cross_realm_access
    ctx = MiniRacerCsim::Context.new
    made = nil
    ctx.attach("mk", lambda {
      made = ctx.create_realm
      made.eval("globalThis.tag = 'frame'")
      made.id
    })
    # Full flow: the outer eval creates the frame via a host fn, then reaches
    # into it with __mr_realmGlobal and writes a property.
    result = ctx.eval(<<~JS)
      var id = mk();
      var g = __mr_realmGlobal(id);
      g.extra = 9;
      g.tag + ":" + g.extra;
    JS
    assert_equal "frame:9", result
    assert_equal 9, made.eval("globalThis.extra")  # the outer write is live in the realm
  end

  def test_unhandled_rejection_fires_per_realm
    ctx = MiniRacerCsim::Context.new(host_namespace: "MiniRacer")
    a = ctx.create_realm
    b = ctx.create_realm
    # Each realm's bridge wires the unhandledrejection hook (csim's role).
    [a, b].each do |r|
      r.eval(<<~JS)
        globalThis.caught = [];
        globalThis.__mr_emitUnhandledRejection = (reason) => { globalThis.caught.push(String(reason)); };
      JS
    end
    b.eval("Promise.reject(new Error('boom'))")
    b.eval("MiniRacer.drainMicrotasks()")
    # fired in B with the reason, and did not leak to A
    assert_equal ["Error: boom"], b.eval("globalThis.caught")
    assert_equal [], a.eval("globalThis.caught")
  end

  def test_handled_rejection_is_not_reported
    ctx = MiniRacerCsim::Context.new(host_namespace: "MiniRacer")
    r = ctx.create_realm
    r.eval(<<~JS)
      globalThis.caught = [];
      globalThis.__mr_emitUnhandledRejection = (reason) => { globalThis.caught.push(String(reason)); };
      const p = Promise.reject(42);
      p.catch(() => {}); // handler added before the checkpoint
    JS
    r.eval("MiniRacer.drainMicrotasks()")
    assert_equal [], r.eval("globalThis.caught")
  end

  def test_realm_of_returns_creation_realm
    ctx = MiniRacerCsim::Context.new
    a = ctx.create_realm
    b = ctx.create_realm
    a.eval("globalThis.B = #{b.id}")
    b.eval("globalThis.fn = function() {}")
    assert_equal b.id, a.eval("__mr_realmOf(__mr_realmGlobal(B).fn)")  # cross-realm
    assert_equal a.id, a.eval("__mr_realmOf(function() {})")
    assert_equal "undefined", a.eval("typeof __mr_realmOf(42)")        # non-object
  end

  def test_realm_of_attributes_callback_to_its_creation_realm
    ctx = MiniRacerCsim::Context.new
    f0 = ctx.create_realm
    f1 = ctx.create_realm
    f0.eval("globalThis.F1 = #{f1.id}")
    # A callback built with frame1's Function constructor, scheduled from frame0:
    # WebIDL attributes its uncaught error to the callback's [[Realm]] = frame1
    # (not the scheduling realm, not the thrown Error's realm). __mr_realmOf
    # reports exactly that, which is what the embedder dispatches the error on.
    assert_equal f1.id, f0.eval(<<~JS)
      const cb = new (__mr_realmGlobal(F1).Function)("throw new Error('x')");
      __mr_realmOf(cb);
    JS
  end

  def test_webassembly
    context = MiniRacerCsim::Context.new()
    context.eval("let instance = null;")
    filename = File.expand_path("../support/add.wasm", __FILE__)
    context.attach("loadwasm", proc { |f| File.read(filename).each_byte.to_a })
    context.attach("print", proc { |f| puts f })

    context.eval <<~JS
      WebAssembly
        .instantiate(new Uint8Array(loadwasm()), {
          wasi_snapshot_preview1: {
            proc_exit: function() { print("exit"); },
            args_get: function() { return 0 },
            args_sizes_get: function() { return 0 }
          }
        })
        .then(i => { instance = i["instance"];})
        .catch(e => print(e.toString()));
    JS

    context.pump_message_loop while !context.eval("instance")

    assert_equal(3, context.eval("instance.exports.add(1,2)"))
  end

  class ReproError < StandardError
    def initialize(response)
      super("response said #{response.code}")
    end
  end

  Response = Struct.new(:code)

  def test_exception_objects
    context = MiniRacerCsim::Context.new
    context.attach("repro", lambda { raise ReproError.new(Response.new(404)) })
    assert_raises(ReproError) { context.eval("repro();") }
  end

  def test_timeout
    context = MiniRacerCsim::Context.new(timeout: 500, max_memory: 20_000_000)
    assert_raises(MiniRacerCsim::ScriptTerminatedError) { context.eval <<~JS }
        var doit = () => {
          while (true) {}
        }
        doit();
        JS
  end

  def test_eval_returns_unfrozen_string
    context = MiniRacerCsim::Context.new
    result = context.eval("'Hello George!'")
    assert_equal("Hello George!", result)
    assert_equal(false, result.frozen?)
  end

  def test_call_returns_unfrozen_string
    context = MiniRacerCsim::Context.new
    context.eval('function hello(name) { return "Hello " + name + "!" }')
    result = context.call("hello", "George")
    assert_equal("Hello George!", result)
    assert_equal(false, result.frozen?)
  end

  def test_callback_string_arguments_are_not_frozen
    context = MiniRacerCsim::Context.new
    context.attach("test", proc { |text| text.frozen? })

    frozen = context.eval("test('Hello George!')")
    assert_equal(false, frozen)
  end

  def test_threading_safety
    Thread.new { MiniRacerCsim::Context.new.eval("100") }.join
    GC.start
  end

  def test_forking
    `bundle exec ruby test/test_forking.rb`
    assert false, "forking test failed" if $?.exitstatus != 0
  end

  def test_poison
    context = MiniRacerCsim::Context.new
    context.eval <<~JS
      const f = () => { throw "poison" }
      const d = {get: f, set: f}
      Object.defineProperty(Array.prototype, "0", d)
      Object.defineProperty(Array.prototype, "1", d)
    JS
    assert_equal 42, context.eval("42")
  end

  def test_map
    context = MiniRacerCsim::Context.new
    expected = { "x" => 42, "y" => 43 }
    assert_equal expected, context.eval("new Map([['x', 42], ['y', 43]])")
    expected = ["x", 42, "y", 43]
    assert_equal expected,
                 context.eval("new Map([['x', 42], ['y', 43]]).entries()")
    expected = %w[x y]
    assert_equal expected,
                 context.eval("new Map([['x', 42], ['y', 43]]).keys()")
    expected = [[42], [43]]
    assert_equal expected,
                 context.eval("new Map([['x', [42]], ['y', [43]]]).values()")
  end

  def test_regexp_string_iterator
    context = MiniRacerCsim::Context.new
    # TODO(bnoordhuis) maybe detect the iterator object and serialize
    # it as a string or array of strings; problem is there is no V8 API
    # to detect regexp string iterator objects
    expected = {}
    assert_equal expected, context.eval("'abc'.matchAll(/./g)")
  end

  def test_function_property
    context = MiniRacerCsim::Context.new
    expected = { "m" => { 1 => 2, 3 => 4 }, "s" => [5, 7, 11, 13], "x" => 42 }
    script = <<~JS
      ({
        f: () => {},
        m: new Map([[1,2],[3,4]]),
        s: new Set([5,7,11,13]),
        x: 42,
      })
    JS
    assert_equal expected, context.eval(script)
  end

  def test_dates_from_active_support
    require "active_support"
    require "active_support/time"
    begin
      Time.zone = "UTC"
    rescue TZInfo::DataSourceNotFound
      skip "no timezone data" # happens on the musl buildbots
    end
    time = Time.current
    context = MiniRacerCsim::Context.new
    context.attach("f", proc { time })
    assert_in_delta time.to_f, context.call("f").to_f, 0.001
  end

  def test_string_encoding
    context = MiniRacerCsim::Context.new
    assert_equal "ä", context.eval("'ä'")
    assert_equal "ok", context.eval("'ok'".encode("ISO-8859-1"))
    assert_equal "ok", context.eval("'ok'".encode("ISO8859-1"))
    assert_equal "ok", context.eval("'ok'".encode("UTF-16LE"))
    assert_equal Encoding::UTF_8, context.eval("'ok'").encoding
    assert_equal Encoding::UTF_8, context.eval("'ok\\uD800\\uDC00'").encoding
    # unmatched surrogate pair, cannot be converted by ruby
    assert_equal Encoding::UTF_16LE, context.eval("'ok\\uD800'").encoding
  end

  def test_object_ref
    context = MiniRacerCsim::Context.new
    context.eval("function f(o) { return o }")
    expected = {}
    expected["a"] = expected["b"] = { "x" => 42 }
    actual = context.call("f", expected)
    assert_equal actual, expected
  end

  def test_termination_exception
    context = MiniRacerCsim::Context.new
    a = Thread.new { context.stop while true }
    b = Thread.new { context.heap_stats while true } # should not crash/abort
    sleep 1.5
    a.kill
    b.kill
  end

  def test_ruby_exception
    context = MiniRacerCsim::Context.new
    context.attach("test", proc { raise "boom" })
    actual = context.eval("try { test() } catch (e) { e }")
    assert_equal(actual.class, MiniRacerCsim::ScriptError)
    assert_equal(actual.message, "boom")
    assert_equal(actual.backtrace, ["JavaScript Error: boom", "JavaScript at <eval>:1:7"])
  end

  def test_large_integer
    [10_000_000_001, -2**63, 2**63-1].each { |big_int|
      context = MiniRacerCsim::Context.new
      context.attach("test", proc { big_int })
      result = context.eval("test()")
      assert_equal(result.class, big_int.class)
      assert_equal(result, big_int)
    }
    types = []
    [2**63/1024-1, 2**63/1024, -2**63/1024+1, -2**63/1024].each { |big_int|
      context = MiniRacerCsim::Context.new
      context.attach("test", proc { big_int })
      context.attach("type", proc { |arg| types.push(arg) })
      result = context.eval("const t = test(); type(typeof t); t")
      assert_equal(result.class, big_int.class)
      assert_equal(result, big_int)
    }
    assert_equal(types, %w[number bigint number bigint])
  end

  def test_uint8array_is_converted_to_string
    context = MiniRacerCsim::Context.new
    result = context.eval('new Uint8Array([0, 1, 2, 3])')
    assert_equal "\x00\x01\x02\x03".b, result
  end

  def test_binary_returns_uint8array
    context = MiniRacerCsim::Context.new
    context.attach("create_uint8_array", -> {
      MiniRacerCsim::Binary.new([1, 2, 3, 4].pack("C*"))
    })

    result = context.eval <<~JS
      var output = create_uint8_array();
      (output instanceof Uint8Array) && Array.from(output).join(",") === "1,2,3,4";
    JS
    assert_equal true, result
  end

  def test_exception_message_encoding
    e = nil
    begin
      MiniRacerCsim::Context.new.eval("throw Error('ä')")
    rescue MiniRacerCsim::RuntimeError => e_
      e = e_
    end
    assert e
    assert_equal(e.message.encoding.to_s, "UTF-8")
  end

  def test_v8_cached_data_version_tag
    # Triggers v8_once_init which is when the constant is populated.
    MiniRacerCsim::Context.new

    assert_kind_of Integer, MiniRacerCsim::V8_CACHED_DATA_VERSION_TAG
    refute_equal 0, MiniRacerCsim::V8_CACHED_DATA_VERSION_TAG
    # Stable across calls.
    assert_equal MiniRacerCsim::V8_CACHED_DATA_VERSION_TAG, MiniRacerCsim::V8_CACHED_DATA_VERSION_TAG
  end

  def test_compile_run_roundtrip
    ctx = MiniRacerCsim::Context.new
    script = ctx.compile("1 + 2 + 3")
    assert_kind_of MiniRacerCsim::Script, script
    assert_equal 6, script.run
    assert_equal 6, script.run # idempotent
  end

  def test_compile_filename_in_parse_error
    err = assert_raises(MiniRacerCsim::ParseError) do
      MiniRacerCsim::Context.new.compile("function foo(", filename: "bundle.js")
    end
    assert_includes err.message, "bundle.js"
  end

  def test_compile_invalid_source
    assert_raises(MiniRacerCsim::ParseError) do
      MiniRacerCsim::Context.new.compile("foo bar baz garbage")
    end
  end

  def test_compile_runtime_error
    ctx = MiniRacerCsim::Context.new
    script = ctx.compile("throw new Error('boom')")
    err = assert_raises(MiniRacerCsim::RuntimeError) do
      script.run
    end
    assert_includes err.message, "boom"
  end

  def test_compile_cached_data_save_restore

    src = "function sq(x) { return x * x } sq(7)"
    ctx_a = MiniRacerCsim::Context.new
    s_a = ctx_a.compile(src, filename: "sq.js", produce_cache: true)
    blob = s_a.cached_data
    assert_kind_of String, blob
    assert_equal Encoding::ASCII_8BIT, blob.encoding
    assert_operator blob.bytesize, :>, 0
    refute_predicate s_a, :cache_rejected?
    assert_equal 49, s_a.run
    ctx_a.dispose

    ctx_b = MiniRacerCsim::Context.new
    s_b = ctx_b.compile(src, filename: "sq.js", cached_data: blob)
    refute_predicate s_b, :cache_rejected?
    assert_nil s_b.cached_data, "accepted blob → nil so caller skips redundant persist"
    assert_equal 49, s_b.run
  end

  def test_compile_cached_data_rejection

    src = "function sq(x) { return x * x } sq(7)"
    corrupt = ("garbage" * 100).b
    ctx = MiniRacerCsim::Context.new
    script = ctx.compile(src, cached_data: corrupt, produce_cache: true)
    assert_predicate script, :cache_rejected?
    fresh = script.cached_data
    assert_kind_of String, fresh
    assert_operator fresh.bytesize, :>, 0
    assert_equal 49, script.run
  end

  def test_compile_default_skips_cache_production

    ctx = MiniRacerCsim::Context.new
    script = ctx.compile("1 + 1", filename: "no_cache.js")
    assert_nil script.cached_data
    refute_predicate script, :cache_rejected?
    assert_equal 2, script.run
  end

  def test_compile_produce_cache_inside_host_fn_raises

    ctx = MiniRacerCsim::Context.new
    caught = nil
    ctx.attach("trigger", lambda {
      begin
        ctx.compile("var v = 1; v", filename: "inside.js", produce_cache: true)
      rescue MiniRacerCsim::RuntimeError => e
        caught = e
      end
      nil
    })
    ctx.eval("trigger()")
    refute_nil caught, "produce_cache: true from a host-fn callback should raise"
    assert_includes caught.message, "host-function callback"
  end

  def test_compile_inside_host_fn_default_is_safe

    # Without produce_cache, repeated re-entrant compiles must not crash —
    # this is the path Discourse-style embedders take from their inline-script
    # host fns. cached_data stays nil because we skip CreateCodeCache in
    # callback frames; the user can warm the cache via top-level compile
    # calls with produce_cache: true at startup instead.
    ctx = MiniRacerCsim::Context.new
    ctx.attach("run_inline", lambda {|label, body|
      script = ctx.compile(body, filename: label)
      script.run
      script.dispose
      nil
    })
    3.times do |i|
      ctx.eval("run_inline('inline://#{i}.js', 'var x = #{i}; x + 1;')")
    end
  end

  def test_compile_cached_data_must_be_binary
    ctx = MiniRacerCsim::Context.new
    assert_raises(EncodingError) do
      ctx.compile("1+1", cached_data: "not binary".encode("UTF-8"))
    end
  end

  def test_compile_cached_data_type_error
    ctx = MiniRacerCsim::Context.new
    assert_raises(TypeError) do
      ctx.compile("1+1", cached_data: 42)
    end
  end

  def test_script_dispose_idempotent
    ctx = MiniRacerCsim::Context.new
    script = ctx.compile("1 + 1")
    assert_equal 2, script.run
    refute_predicate script, :disposed?
    script.dispose
    assert_predicate script, :disposed?
    assert_nil script.dispose
    assert_raises(MiniRacerCsim::RuntimeError) { script.run }
  end

  def test_script_after_context_dispose
    ctx = MiniRacerCsim::Context.new
    script = ctx.compile("1 + 1", produce_cache: true)
    ctx.dispose
    assert_raises(MiniRacerCsim::ContextDisposedError) { script.run }
    # cached_data is still readable — it was stashed at compile time
    refute_nil script.cached_data
    # dispose on script after context dispose is a no-op
    assert_nil script.dispose
  end

  def test_script_new_direct_raises
    assert_raises(StandardError) { MiniRacerCsim::Script.new }
  end

  def test_compile_then_eval_interleave
    # Compile mutates the context's globals just like eval, and survives
    # interleaved evals.
    ctx = MiniRacerCsim::Context.new
    s1 = ctx.compile("var counter = 10; counter")
    assert_equal 10, s1.run
    ctx.eval("counter += 5")
    s2 = ctx.compile("counter * 2")
    assert_equal 30, s2.run
    assert_equal 31, ctx.eval("counter + 16")
  end

  # -- ES module API (Context#compile_module / MiniRacerCsim::Module) --

  def test_compile_module_returns_handle
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    assert_kind_of MiniRacerCsim::Module, mod
    refute_predicate mod, :disposed?
  end

  def test_compile_module_parse_error
    ctx = MiniRacerCsim::Context.new
    assert_raises(MiniRacerCsim::ParseError) do
      ctx.compile_module("this is not valid", filename: "bad.js")
    end
  end

  def test_compile_module_accepts_import_declaration
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("import { x } from 'other'; export const y = x + 1",
                             filename: "imp.js")
    assert_kind_of MiniRacerCsim::Module, mod
  end

  def test_module_new_direct_raises
    assert_raises(MiniRacerCsim::RuntimeError) { MiniRacerCsim::Module.new }
  end

  def test_module_dispose_idempotent
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    mod.dispose
    assert_predicate mod, :disposed?
    assert_nil mod.dispose
  end

  def test_module_after_context_dispose
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    ctx.dispose
    assert_raises(MiniRacerCsim::ContextDisposedError) { mod.status }
    assert_nil mod.dispose
  end

  def test_module_status_starts_uninstantiated
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    assert_equal :uninstantiated, mod.status
  end

  def test_module_status_after_dispose_raises
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    mod.dispose
    assert_raises(MiniRacerCsim::RuntimeError) { mod.status }
  end

  def test_module_instantiate_no_imports_skips_resolver
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    mod.instantiate { raise "resolver should not be called" }
    assert_equal :instantiated, mod.status
  end

  def test_module_instantiate_calls_resolver_with_specifier
    ctx = MiniRacerCsim::Context.new
    dep = ctx.compile_module("export const y = 7", filename: "dep.js")
    main = ctx.compile_module("import { y } from 'dep'; export const z = y * 2",
                              filename: "main.js")
    seen = []
    main.instantiate {|spec| seen << spec; dep }
    assert_equal ["dep"], seen
    assert_equal :instantiated, main.status
    assert_equal :instantiated, dep.status
  end

  def test_module_instantiate_requires_block
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    assert_raises(ArgumentError) { mod.instantiate }
  end

  def test_module_instantiate_resolver_returning_non_module_raises
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("import 'foo'", filename: "m.js")
    err = assert_raises(MiniRacerCsim::RuntimeError) do
      mod.instantiate {|_spec| "not a module" }
    end
    assert_includes err.message, "MiniRacerCsim::Module"
  end

  def test_module_instantiate_resolver_exception_bubbles_original_class
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("import 'foo'", filename: "m.js")
    err = assert_raises(ArgumentError) do
      mod.instantiate {|_spec| raise ArgumentError, "deliberate" }
    end
    assert_equal "deliberate", err.message
  end

  def test_module_instantiate_resolves_transitively
    ctx = MiniRacerCsim::Context.new
    a = ctx.compile_module("export const a = 1", filename: "a.js")
    b = ctx.compile_module("import { a } from 'a'; export const b = a + 1", filename: "b.js")
    c = ctx.compile_module("import { b } from 'b'; export const c = b + 1", filename: "c.js")
    table = { "a" => a, "b" => b }
    seen = []
    c.instantiate {|spec| seen << spec; table.fetch(spec) }
    assert_equal ["a", "b"], seen.sort
    [a, b, c].each {|m| assert_equal :instantiated, m.status }
  end

  def test_module_instantiate_passes_referrer_url_to_resolver
    ctx = MiniRacerCsim::Context.new
    dep = ctx.compile_module("export const y = 1", filename: "lib/dep.js")
    main = ctx.compile_module("import { y } from './dep'; export const z = y",
                              filename: "lib/main.js")
    seen = []
    main.instantiate {|spec, referrer|
      seen << [spec, referrer]
      dep
    }
    assert_equal [['./dep', 'lib/main.js']], seen
  end

  def test_module_import_meta_url_is_populated
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("globalThis.metaUrl = import.meta.url",
                             filename: 'src/entry.js')
    mod.instantiate { raise 'resolver should not be called' }
    mod.evaluate
    assert_equal 'src/entry.js', ctx.eval('globalThis.metaUrl')
  end

  def test_module_filename_with_embedded_nul_survives_roundtrip
    ctx = MiniRacerCsim::Context.new
    name = "a\0b.js"
    mod = ctx.compile_module('globalThis.url = import.meta.url', filename: name)
    mod.instantiate { raise 'resolver should not be called' }
    mod.evaluate
    assert_equal name, ctx.eval('globalThis.url')
  end

  def test_module_evaluate_before_instantiate_raises
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module('export const x = 1', filename: 'a.js')
    err = assert_raises(MiniRacerCsim::RuntimeError) { mod.evaluate }
    assert_includes err.message, 'instantiated'
  end

  def test_module_resolver_rejects_module_from_other_context
    ctx_a = MiniRacerCsim::Context.new
    ctx_b = MiniRacerCsim::Context.new
    foreign = ctx_a.compile_module('export const x = 1', filename: 'a.js')
    main = ctx_b.compile_module("import 'foo'", filename: 'main.js')
    err = assert_raises(MiniRacerCsim::RuntimeError) do
      main.instantiate {|_spec, _ref| foreign }
    end
    assert_includes err.message, 'different Context'
  end

  def test_dynamic_import_resolver_resolves_with_namespace
    ctx = MiniRacerCsim::Context.new
    dep = ctx.compile_module('export const x = 42', filename: 'dep.js')
    dep.instantiate { raise 'resolver should not be called' }
    dep.evaluate
    seen = []
    ctx.dynamic_import_resolver = ->(spec, ref) {
      seen << [spec, ref]
      dep
    }
    ctx.eval("import('dep').then(ns => { globalThis.r = ns.x })",
             filename: 'caller.js')
    ctx.perform_microtask_checkpoint
    assert_equal [['dep', 'caller.js']], seen
    assert_equal 42, ctx.eval('globalThis.r')
  end

  def test_dynamic_import_without_resolver_rejects
    ctx = MiniRacerCsim::Context.new
    ctx.eval(<<~JS, filename: 'main.js')
      import('foo').catch(e => { globalThis.err = String(e) })
    JS
    ctx.perform_microtask_checkpoint
    assert_match(/dynamic_import_resolver/, ctx.eval('globalThis.err'))
  end

  def test_dynamic_import_resolver_rejects_module_from_other_context
    ctx_a = MiniRacerCsim::Context.new
    ctx_b = MiniRacerCsim::Context.new
    foreign = ctx_a.compile_module('export const x = 1', filename: 'a.js')
    foreign.instantiate { raise 'noop' }
    foreign.evaluate
    ctx_b.dynamic_import_resolver = ->(_spec, _ref) { foreign }
    ctx_b.eval(<<~JS, filename: 'main.js')
      import('foo').catch(e => { globalThis.err = String(e) })
    JS
    ctx_b.perform_microtask_checkpoint
    assert_match(/different Context/, ctx_b.eval('globalThis.err'))
  end

  def test_dynamic_import_resolver_auto_evaluates_pending_module
    ctx = MiniRacerCsim::Context.new
    dep = ctx.compile_module('globalThis.evaled = (globalThis.evaled||0)+1; export const x = 7',
                             filename: 'dep.js')
    dep.instantiate { raise 'noop' }
    # Note: we do NOT call dep.evaluate here — the dynamic import path must drive it.
    ctx.dynamic_import_resolver = ->(_spec, _ref) { dep }
    ctx.eval("import('dep').then(ns => { globalThis.r = ns.x })",
             filename: 'caller.js')
    ctx.perform_microtask_checkpoint
    assert_equal 7, ctx.eval('globalThis.r')
    assert_equal 1, ctx.eval('globalThis.evaled')
  end

  def test_dynamic_import_auto_evaluate_drains_module_body_microtasks
    ctx = MiniRacerCsim::Context.new
    dep = ctx.compile_module(<<~JS, filename: 'dep.js')
      globalThis.t = 0;
      Promise.resolve().then(() => { globalThis.t = 1 });
      export const x = 1;
    JS
    dep.instantiate { raise 'noop' }
    # Do NOT call dep.evaluate — the dynamic import path drives evaluation,
    # and must drain microtasks so the module-body .then settles before we
    # resolve the outer import() Promise.
    ctx.dynamic_import_resolver = ->(_s, _r) { dep }
    ctx.eval("import('dep').then(() => { globalThis.observed = globalThis.t })",
             filename: 'caller.js')
    ctx.perform_microtask_checkpoint
    assert_equal 1, ctx.eval('globalThis.observed')
  end

  def test_dynamic_import_resolver_setter_type_check
    ctx = MiniRacerCsim::Context.new
    assert_raises(TypeError) { ctx.dynamic_import_resolver = 42 }
    ctx.dynamic_import_resolver = nil
    assert_nil ctx.dynamic_import_resolver
    blk = ->(_s, _r) { nil }
    ctx.dynamic_import_resolver = blk
    assert_equal blk, ctx.dynamic_import_resolver
  end

  def test_module_dispose_releases_handles_eagerly
    ctx = MiniRacerCsim::Context.new
    before = ctx.heap_stats[:used_heap_size]
    1000.times do |i|
      m = ctx.compile_module("export const x = #{i}", filename: "m#{i}.js")
      m.dispose
    end
    ctx.low_memory_notification
    after = ctx.heap_stats[:used_heap_size]
    assert_operator after - before, :<, 2_000_000,
                    "expected eager dispose to keep heap growth bounded (was #{after - before} bytes)"
  end

  def test_module_instantiate_resolver_can_compile_lazily
    ctx = MiniRacerCsim::Context.new
    sources = {
      "tip"  => "import { mid } from 'mid'; export const tip = mid + 'tip'",
      "mid"  => "import { base } from 'base'; export const mid = base + 'mid'",
      "base" => "export const base = 'base'",
    }
    cache = {}
    tip = ctx.compile_module(sources["tip"], filename: "tip.js")
    tip.instantiate {|spec|
      cache[spec] ||= ctx.compile_module(sources.fetch(spec), filename: "#{spec}.js")
    }
    assert_equal ["base", "mid"], cache.keys.sort
    assert_equal :instantiated, tip.status
  end

  def test_module_evaluate_returns_undefined_for_simple_module
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 42", filename: "a.js")
    mod.instantiate { raise "resolver should not be called" }
    assert_nil mod.evaluate
  end

  def test_module_evaluate_runtime_error_propagates
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("throw new Error('boom')", filename: "bad.js")
    mod.instantiate { raise "resolver should not be called" }
    err = assert_raises(MiniRacerCsim::RuntimeError) { mod.evaluate }
    assert_includes err.message, "boom"
  end

  def test_module_evaluate_top_level_await_unsupported
    ctx = MiniRacerCsim::Context.new
    tla = ctx.compile_module(
      "await new Promise((resolve) => setTimeout(resolve, 1)); export const x = 1",
      filename: "tla.js")
    tla.instantiate { raise "resolver should not be called" }
    # setTimeout is undefined in this isolate so the await rejects; the
    # surface contract is that the user gets a MiniRacerCsim::RuntimeError
    # rather than a hang/crash, regardless of which rejection V8 surfaces.
    assert_raises(MiniRacerCsim::RuntimeError) { tla.evaluate }
  end

  def test_module_namespace_data_exports
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module(
      "export const a = 1; export const b = 'two'; export const c = [3, 4]",
      filename: "a.js")
    mod.instantiate { raise "resolver should not be called" }
    mod.evaluate
    assert_equal({"a" => 1, "b" => "two", "c" => [3, 4]}, mod.namespace)
  end

  def test_module_namespace_includes_default_export
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module(
      "const v = { kind: 'obj', n: 7 }; export default v",
      filename: "d.js")
    mod.instantiate { raise "resolver should not be called" }
    mod.evaluate
    assert_equal({"default" => {"kind" => "obj", "n" => 7}}, mod.namespace)
  end

  def test_module_namespace_before_instantiate_raises
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    assert_raises(MiniRacerCsim::RuntimeError) { mod.namespace }
  end

  def test_module_status_transitions
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const v = 5", filename: "a.js")
    assert_equal :uninstantiated, mod.status
    mod.instantiate { raise "resolver should not be called" }
    assert_equal :instantiated, mod.status
    mod.evaluate
    assert_equal :evaluated, mod.status
  end

  def test_module_imports_provide_runtime_bindings
    ctx = MiniRacerCsim::Context.new
    dep = ctx.compile_module("export const base = 10", filename: "dep.js")
    main = ctx.compile_module(
      "import { base } from 'dep'; export const doubled = base * 2",
      filename: "main.js")
    main.instantiate {|spec| dep }
    dep.evaluate
    main.evaluate
    assert_equal 20, main.namespace["doubled"]
  end

  def test_module_evaluate_idempotent
    ctx = MiniRacerCsim::Context.new
    mod = ctx.compile_module("export const x = 1", filename: "a.js")
    mod.instantiate { raise "resolver should not be called" }
    mod.evaluate
    assert_nil mod.evaluate
  end

  def test_module_accumulation_freed_by_context_dispose
    # Many short-lived modules accumulate handles until ctx.dispose, which
    # walks st.modules under the still-live isolate before tearing it down.
    ctx = MiniRacerCsim::Context.new
    100.times {|i| ctx.compile_module("export const v#{i} = #{i}", filename: "m#{i}.js") }
    ctx.dispose
  end

  # ---- load_module_graph (batched ESM graph load) ----

  # Resolves "./x.js" against any referrer to "/x.js"; serves from a source map.
  def graph_loader(sources, fetch_calls: nil, resolve_calls: nil)
    fetch = lambda do |urls|
      fetch_calls << urls if fetch_calls
      urls.map {|u| (s = sources[u]) ? [s, nil] : nil }
    end
    resolve = lambda do |edges|
      resolve_calls << edges if resolve_calls
      edges.map {|specifier, _referrer| specifier.start_with?("./") ? "/#{specifier[2..]}" : specifier }
    end
    [resolve, fetch]
  end

  def test_load_module_graph_evaluates_whole_graph
    ctx = MiniRacerCsim::Context.new
    sources = {
      "/app.js" => 'import {a} from "./a.js"; import {b} from "./b.js"; globalThis.RESULT = a + b;',
      "/a.js"   => 'import {c} from "./c.js"; export const a = c + 1;',
      "/b.js"   => "export const b = 20;",
      "/c.js"   => "export const c = 100;",
    }
    resolve, fetch = graph_loader(sources)
    result = ctx.load_module_graph("/app.js", resolve: resolve, fetch_batch: fetch)

    assert_equal(121, ctx.eval("globalThis.RESULT"))
    assert_equal(%w[/a.js /app.js /b.js /c.js], result[:modules].map {|m| m[:url] }.sort)
    assert_equal([false], result[:modules].map {|m| m[:cache_rejected] }.uniq)
  end

  def test_load_module_graph_batches_callbacks_per_level
    ctx = MiniRacerCsim::Context.new
    sources = {
      "/app.js" => 'import {a} from "./a.js"; import {b} from "./b.js";',
      "/a.js"   => 'import {c} from "./c.js"; export const a = 1;',
      "/b.js"   => "export const b = 2;",
      "/c.js"   => "export const c = 3;",
    }
    fetch_calls = []
    resolve_calls = []
    resolve, fetch = graph_loader(sources, fetch_calls: fetch_calls, resolve_calls: resolve_calls)
    ctx.load_module_graph("/app.js", resolve: resolve, fetch_batch: fetch)

    # 4 modules + 3 imports would be 7 crossings one-at-a-time; batched per graph
    # level it is 3 fetch + 2 resolve = 5, and the second fetch carries 2 URLs.
    assert_equal(3, fetch_calls.size)
    assert_equal(2, resolve_calls.size)
    assert_includes(fetch_calls, %w[/a.js /b.js])
  end

  def test_load_module_graph_populates_import_meta_url
    ctx = MiniRacerCsim::Context.new
    resolve, fetch = graph_loader({ "/m.js" => "globalThis.META = import.meta.url; export const x = 1;" })
    ctx.load_module_graph("/m.js", resolve: resolve, fetch_batch: fetch)
    assert_equal("/m.js", ctx.eval("globalThis.META"))
  end

  def test_load_module_graph_missing_dependency_fails_gracefully
    ctx = MiniRacerCsim::Context.new
    # fetch returns nil for the (resolved) missing dependency.
    resolve, fetch = graph_loader({ "/e.js" => 'import {z} from "./missing.js"; export const y = z;' })
    assert_raises(MiniRacerCsim::RuntimeError) do
      ctx.load_module_graph("/e.js", resolve: resolve, fetch_batch: fetch)
    end
    # The context must remain usable after a failed graph load.
    assert_equal(2, ctx.eval("1 + 1"))
  end

  def test_load_module_graph_consumes_cached_data
    body = "export const v = 42;"
    cache = MiniRacerCsim::Context.new.compile_module(body, filename: "/v.js", produce_cache: true).cached_data
    refute_nil(cache)

    ctx = MiniRacerCsim::Context.new
    result = ctx.load_module_graph("/v.js",
      resolve: ->(edges) { edges.map { nil } },
      fetch_batch: ->(urls) { urls.map {|u| u == "/v.js" ? [body, cache] : nil } })
    # A matching code cache is accepted (not rejected), so csim's cross-process
    # bytecode cache keeps working through the batched path.
    refute(result[:modules][0][:cache_rejected])
  end

  def test_load_module_graph_reports_rejected_cache
    # A code cache produced for one source is rejected when consumed against a
    # different source (the cross-process-cache version/content mismatch case).
    stale = MiniRacerCsim::Context.new.compile_module("export const v = 1;",
                                                  filename: "/v.js", produce_cache: true).cached_data
    ctx = MiniRacerCsim::Context.new
    result = ctx.load_module_graph("/v.js",
      resolve: ->(edges) { edges.map { nil } },
      fetch_batch: ->(urls) { urls.map { ["export const somethingElse = 2;", stale] } })
    assert(result[:modules][0][:cache_rejected])
  end

  def test_load_module_graph_propagates_callback_exception
    ctx = MiniRacerCsim::Context.new
    boom = Class.new(StandardError)
    assert_raises(boom) do
      ctx.load_module_graph("/x.js",
        resolve: ->(_e) { [] },
        fetch_batch: ->(_u) { raise boom, "fetch failed" })
    end
    assert_equal(2, ctx.eval("1 + 1"))
  end

  def test_load_module_graph_requires_callables
    ctx = MiniRacerCsim::Context.new
    assert_raises(ArgumentError) { ctx.load_module_graph("/x.js", resolve: nil, fetch_batch: ->(u) { [] }) }
    assert_raises(ArgumentError) { ctx.load_module_graph("/x.js", resolve: ->(e) { [] }, fetch_batch: nil) }
  end

  def test_load_module_graph_handles_cyclic_imports
    ctx = MiniRacerCsim::Context.new
    # a.js <-> b.js form a legal ES module cycle.
    sources = {
      "/a.js" => 'import {fromB} from "./b.js"; export const fromA = "A"; globalThis.SEEN = (globalThis.SEEN || "") + "a";',
      "/b.js" => 'import {fromA} from "./a.js"; export const fromB = "B"; globalThis.SEEN = (globalThis.SEEN || "") + "b";',
    }
    resolve, fetch = graph_loader(sources)
    result = ctx.load_module_graph("/a.js", resolve: resolve, fetch_batch: fetch)
    # Each module compiled/evaluated exactly once despite the cycle.
    assert_equal(%w[/a.js /b.js], result[:modules].map {|m| m[:url] }.sort)
    assert_equal("ba", ctx.eval("globalThis.SEEN"))
  end

  def test_load_module_graph_fetches_shared_dependency_once
    ctx = MiniRacerCsim::Context.new
    # Diamond: app -> {left, right} -> shared. shared must be fetched once.
    sources = {
      "/app.js"    => 'import "./left.js"; import "./right.js";',
      "/left.js"   => 'import "./shared.js"; export const l = 1;',
      "/right.js"  => 'import "./shared.js"; export const r = 2;',
      "/shared.js" => "globalThis.SHARED = (globalThis.SHARED || 0) + 1;",
    }
    fetch_calls = []
    resolve, fetch = graph_loader(sources, fetch_calls: fetch_calls)
    result = ctx.load_module_graph("/app.js", resolve: resolve, fetch_batch: fetch)

    assert_equal(1, ctx.eval("globalThis.SHARED")) # evaluated once
    assert_equal(1, result[:modules].count {|m| m[:url] == "/shared.js" }) # listed once
    assert_equal(1, fetch_calls.flatten.count("/shared.js")) # fetched once
  end

  def test_load_module_graph_dynamic_import_reuses_loaded_module
    ctx = MiniRacerCsim::Context.new
    # The entry statically imports shared and mutates it; a later dynamic
    # import() of the same URL must see the SAME Module instance (n == 42), not
    # a freshly recompiled one (n == 0). This is the identity contract.
    sources = {
      "/entry.js" => <<~JS,
        import {state} from "./shared.js";
        state.n = 42;
        globalThis.OUT = "pending";
        import("./shared.js").then(m => { globalThis.OUT = m.state.n; });
      JS
      "/shared.js" => "export const state = { n: 0 };",
    }
    fetch_calls = []
    resolve, fetch = graph_loader(sources, fetch_calls: fetch_calls)
    ctx.load_module_graph("/entry.js", resolve: resolve, fetch_batch: fetch)

    assert_equal(42, ctx.eval("globalThis.OUT"))
    # shared.js was fetched once (the dynamic import reused it, no re-fetch).
    assert_equal(1, fetch_calls.flatten.count("/shared.js"))
  end

  def test_load_module_graph_dynamic_import_loads_new_subgraph
    ctx = MiniRacerCsim::Context.new
    # lazy.js is not in the static graph; the runtime dynamic import must walk
    # and load it on demand via the persisted fetch/resolve callbacks.
    sources = {
      "/entry.js" => <<~JS,
        globalThis.OUT = "pending";
        import("./lazy.js").then(m => { globalThis.OUT = m.value; });
      JS
      "/lazy.js" => "export const value = 99;",
    }
    resolve, fetch = graph_loader(sources)
    ctx.load_module_graph("/entry.js", resolve: resolve, fetch_batch: fetch)
    assert_equal(99, ctx.eval("globalThis.OUT"))
  end

  def test_load_module_graph_relists_only_newly_compiled_modules
    ctx = MiniRacerCsim::Context.new
    sources = {
      "/a.js"      => 'import "./shared.js"; export const a = 1;',
      "/b.js"      => 'import "./shared.js"; export const b = 2;',
      "/shared.js" => "globalThis.S = (globalThis.S || 0) + 1;",
    }
    resolve, fetch = graph_loader(sources)
    r1 = ctx.load_module_graph("/a.js", resolve: resolve, fetch_batch: fetch)
    r2 = ctx.load_module_graph("/b.js", resolve: resolve, fetch_batch: fetch)

    assert_includes(r1[:modules].map {|m| m[:url] }, "/shared.js")
    # The second load reuses the already-registered /shared.js: not relisted and
    # not re-evaluated (S stays 1), so it is the same instance.
    assert_equal(%w[/b.js], r2[:modules].map {|m| m[:url] })
    assert_equal(1, ctx.eval("globalThis.S"))
  end

  def test_load_module_graph_rolls_back_a_failed_load
    ctx = MiniRacerCsim::Context.new
    full = {
      "/entry.js" => 'import {x} from "./dep.js"; globalThis.X = x;',
      "/dep.js"   => "export const x = 7;",
    }
    available = { "/entry.js" => full["/entry.js"] } # dep missing on the first try
    resolve = ->(edges) { edges.map {|spec, _ref| "/#{spec.sub(%r{\A\./}, "")}" } }
    fetch   = ->(urls)  { urls.map  {|u| (s = available[u]) ? [s, nil] : nil } }

    # First load fails: /dep.js 404s, so /entry.js can't be instantiated.
    assert_raises(MiniRacerCsim::RuntimeError) do
      ctx.load_module_graph("/entry.js", resolve: resolve, fetch_batch: fetch)
    end
    # The failed load must not leave a half-loaded /entry.js in the registry: with
    # the dependency now available, a retry recompiles cleanly and succeeds.
    available["/dep.js"] = full["/dep.js"]
    ctx.load_module_graph("/entry.js", resolve: resolve, fetch_batch: fetch)
    assert_equal(7, ctx.eval("globalThis.X"))
  end

  def test_load_module_graph_refuses_reset_realm_from_callback
    ctx = MiniRacerCsim::Context.new
    # reset_realm during a fetch/resolve callback would tear the realm out from
    # under the in-flight graph load; in_callback must make it refuse.
    fetch = lambda do |urls|
      ctx.reset_realm
      urls.map { nil }
    end
    error = assert_raises(MiniRacerCsim::RuntimeError) do
      ctx.load_module_graph("/x.js", resolve: ->(e) { [] }, fetch_batch: fetch)
    end
    assert_match(/within a host function callback/, error.message)
    assert_equal(2, ctx.eval("1 + 1"))
  end
end
