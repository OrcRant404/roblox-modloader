using RML.Core.Tests.Fixtures;
using Roblox;
using Xunit;

namespace RML.Core.Tests;

[Collection("Mod loader")]
public sealed class ModLoaderLifecycleTests
{
    [Fact]
    public void Filtered_Mod_Remains_Loaded_Until_Last_Interested_Model_Closes()
    {
        using var fixture = LoadedFixture.Filtered();
        Assert.Empty(fixture.Events);

        ModLoader.OnDataModelChanged(0, 1, DataModelType.Edit);
        ModLoader.OnDataModelChanged(0, 2, DataModelType.Server);
        ModLoader.OnDataModelChanged(0, 3, DataModelType.Client);
        ModLoader.OnDataModelChanged(3, 0, DataModelType.Client);
        ModLoader.OnDataModelChanged(2, 0, DataModelType.Server);
        Assert.Equal(["load", "loaded:Edit", "loaded:Server", "unloaded:Server"], fixture.Events);

        ModLoader.OnDataModelChanged(1, 0, DataModelType.Edit);
        Assert.Equal(
            ["load", "loaded:Edit", "loaded:Server", "unloaded:Server", "unloaded:Edit", "unload"],
            fixture.Events);

        // An untracked/null notification must not unload a second time.
        ModLoader.OnDataModelChanged(0, 0, DataModelType.Standalone);
        fixture.Unload();
        Assert.Equal(1, fixture.Events.Count(value => value == "unload"));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Explicit_Unload_Or_Shutdown_After_One_Model_Closes_Still_Cleans_Up(bool shutdown)
    {
        using var fixture = LoadedFixture.Filtered();
        ModLoader.OnDataModelChanged(0, 1, DataModelType.Edit);
        ModLoader.OnDataModelChanged(0, 2, DataModelType.Server);
        ModLoader.OnDataModelChanged(2, 0, DataModelType.Server);
        Assert.DoesNotContain("unload", fixture.Events);

        if (shutdown)
            ModLoader.Shutdown();
        else
            fixture.Unload();

        Assert.Equal(["load", "loaded:Edit", "loaded:Server", "unloaded:Server", "unload"], fixture.Events);
    }

    [Fact]
    public void Model_Replacement_Preserves_Mod_Lifetime_And_Last_Close_Allows_Reinitialization()
    {
        using var fixture = LoadedFixture.Filtered();
        ModLoader.OnDataModelChanged(0, 1, DataModelType.Edit);
        ModLoader.OnDataModelChanged(1, 2, DataModelType.Edit);
        ModLoader.OnDataModelChanged(2, 0, DataModelType.Edit);
        ModLoader.OnDataModelChanged(0, 3, DataModelType.Standalone);
        fixture.Unload();

        Assert.Equal(
            ["load", "loaded:Edit", "unloaded:Edit", "loaded:Edit", "unloaded:Edit", "unload",
             "load", "loaded:Standalone", "unload"],
            fixture.Events);
    }

    [Fact]
    public void Global_Mod_Survives_All_Model_Closures_Until_Explicit_Unload()
    {
        using var fixture = LoadedFixture.Global();
        Assert.Equal(["load"], fixture.Events);
        ModLoader.OnDataModelChanged(0, 1, DataModelType.Edit);
        ModLoader.OnDataModelChanged(1, 0, DataModelType.Edit);
        ModLoader.OnDataModelChanged(0, 2, DataModelType.Server);
        ModLoader.OnDataModelChanged(2, 0, DataModelType.Server);
        Assert.DoesNotContain("unload", fixture.Events);
        Assert.Equal(1, fixture.Events.Count(value => value == "load"));

        fixture.Unload();
        Assert.Equal(1, fixture.Events.Count(value => value == "unload"));
    }

    private sealed class LoadedFixture : IDisposable
    {
        private readonly string _directory;
        private readonly string _modPath;
        private readonly string _logPath;
        private readonly string _environmentKey;
        private bool _unloaded;

        private LoadedFixture(Type type, string environmentKey)
        {
            _directory = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "rml_lifecycle_" + Guid.NewGuid());
            System.IO.Directory.CreateDirectory(_directory);
            _modPath = System.IO.Path.Combine(_directory, "fixture.dll");
            _logPath = System.IO.Path.Combine(_directory, "lifecycle.log");
            _environmentKey = environmentKey;
            System.IO.File.Copy(type.Assembly.Location, _modPath);
            Environment.SetEnvironmentVariable(_environmentKey, _logPath);
            ModLoader.LoadMod(_modPath);
        }

        public static LoadedFixture Filtered()
            => new(typeof(FilteredLifecycleMod), "RML_TEST_FILTERED_LIFECYCLE_LOG");

        public static LoadedFixture Global()
            => new(typeof(ConcurrencyFakeMod), "RML_TEST_LIFECYCLE_LOG");

        public string[] Events => System.IO.File.Exists(_logPath) ? System.IO.File.ReadAllLines(_logPath) : [];

        public void Unload()
        {
            if (_unloaded)
                return;

            ModLoader.UnloadMod(_modPath);
            _unloaded = true;
        }

        public void Dispose()
        {
            Unload();
            Environment.SetEnvironmentVariable(_environmentKey, null);
            System.IO.Directory.Delete(_directory, true);
        }
    }
}
