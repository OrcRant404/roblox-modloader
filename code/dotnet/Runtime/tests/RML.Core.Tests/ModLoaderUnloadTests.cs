using System.Runtime.CompilerServices;
using System.Runtime.Loader;

using RML.Core.Tests.Fixtures;
using RML.Logging;

using Roblox;

using Xunit;

namespace RML.Core.Tests;

[Collection("Mod loader")]
public sealed class ModLoaderUnloadTests
{
    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public void Actual_Unload_Path_Collects_Mod_Without_A_False_Rooted_Warning(bool shutdown, bool initialized)
    {
        using var fixture = new LoadedFixture();
        using var logs = new CapturingSink(fixture.ModPath);
        if (initialized)
            ModLoader.OnDataModelChanged(0, 1, DataModelType.Edit);
        var context = FindWeakContext(fixture.ModPath);

        if (shutdown)
            ModLoader.Shutdown();
        else
            ModLoader.UnloadMod(fixture.ModPath);

        Assert.DoesNotContain(logs.Messages, message => message.Contains("still rooted"));
        Assert.False(context.IsAlive, "The actual ModLoader unload GC loop should collect an unreferenced mod context.");
        Assert.Equal(initialized ? 1 : 0, fixture.Events.Count(value => value == "unload"));

        // The same path is removed from the registry, and a second unload only
        // reports a missing mod instead of invoking its lifecycle a second time.
        ModLoader.UnloadMod(fixture.ModPath);
        Assert.Contains(logs.Messages, message => message == $"No mod loaded from path {fixture.ModPath}.");
        Assert.Equal(initialized ? 1 : 0, fixture.Events.Count(value => value == "unload"));
    }

    [Fact]
    public void Actual_Unload_Path_Still_Reports_An_Externally_Rooted_Context()
    {
        using var fixture = new LoadedFixture();
        using var logs = new CapturingSink(fixture.ModPath);
        using var pin = PinContext(fixture.ModPath);

        ModLoader.UnloadMod(fixture.ModPath);

        Assert.True(pin.Weak.IsAlive);
        Assert.Contains(logs.Messages, message => message.Contains("still rooted"));
        pin.Release();
        Collect(pin.Weak);
        Assert.False(pin.Weak.IsAlive, "The deliberately retained context should collect after its external root is released.");
        GC.KeepAlive(pin);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference FindWeakContext(string path)
        => new(AssemblyLoadContext.All.Single(context => context.Name == System.IO.Path.GetFileNameWithoutExtension(path)));

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static ContextPin PinContext(string path)
        => new(AssemblyLoadContext.All.Single(context => context.Name == System.IO.Path.GetFileNameWithoutExtension(path)));

    private static void Collect(WeakReference context)
    {
        for (var i = 0; context.IsAlive && i < 10; ++i)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
    }

    private sealed class ContextPin(AssemblyLoadContext context) : IDisposable
    {
        private AssemblyLoadContext? _context = context;
        public WeakReference Weak { get; } = new(context);

        [MethodImpl(MethodImplOptions.NoInlining)]
        public void Release()
        {
            GC.KeepAlive(_context);
            _context = null;
        }

        public void Dispose() => Release();
    }

    private sealed class CapturingSink : ILogSink, IDisposable
    {
        private readonly string _path;
        public List<string> Messages { get; } = [];

        public CapturingSink(string path)
        {
            _path = path;
            Log.AddSink(this);
        }

        public void Emit(in LogEvent logEvent)
        {
            // Copy only text; retaining provider exceptions or other objects in
            // the assertion sink would itself keep the collectible mod alive.
            if (logEvent.Message.Contains(_path))
                Messages.Add(logEvent.Message);
        }

        public void Dispose() => Log.RemoveSink(this);
    }

    private sealed class LoadedFixture : IDisposable
    {
        private readonly string _directory;
        private readonly string _logPath;
        public string ModPath { get; }
        public string[] Events => System.IO.File.Exists(_logPath) ? System.IO.File.ReadAllLines(_logPath) : [];

        public LoadedFixture()
        {
            _directory = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "rml_unload_" + Guid.NewGuid());
            System.IO.Directory.CreateDirectory(_directory);
            ModPath = System.IO.Path.Combine(_directory, "unload_" + Guid.NewGuid() + ".dll");
            _logPath = System.IO.Path.Combine(_directory, "lifecycle.log");
            System.IO.File.Copy(typeof(FilteredLifecycleMod).Assembly.Location, ModPath);
            Environment.SetEnvironmentVariable("RML_TEST_FILTERED_LIFECYCLE_LOG", _logPath);
            ModLoader.LoadMod(ModPath);
        }

        public void Dispose()
        {
            ModLoader.UnloadMod(ModPath);
            Environment.SetEnvironmentVariable("RML_TEST_FILTERED_LIFECYCLE_LOG", null);
            System.IO.Directory.Delete(_directory, true);
        }
    }
}
