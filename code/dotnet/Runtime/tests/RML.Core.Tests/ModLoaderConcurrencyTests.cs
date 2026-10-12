using RML.Core.Api;
using RML.Core.Modding;

using Roblox;

using Xunit;

namespace RML.Core.Tests;

[Mod("rml.tests.concurrency-fake-mod", "1.0")]
public sealed class ConcurrencyFakeMod : IMod, IDataModelAware
{
    private static string? LifecycleLog => Environment.GetEnvironmentVariable("RML_TEST_LIFECYCLE_LOG");

    public int OnLoad()
    {
        AppendLifecycle("load");
        return 0;
    }

    public void OnUnload()
    {
        AppendLifecycle("unload");
    }

    public void OnDataModelLoaded(DataModel game, DataModelType dataModelType)
    {
        AppendLifecycle("data-model-loaded");
    }

    public void OnDataModelUnloaded(DataModel game, DataModelType dataModelType)
    {
        AppendLifecycle("data-model-unloaded");
    }

    private static void AppendLifecycle(string entry)
    {
        var path = LifecycleLog;
        if (path != null)
            System.IO.File.AppendAllLines(path, [entry]);
    }
}

[Collection("Mod loader")]
public class ModLoaderConcurrencyTests
{
    [Fact]
    public void Replacing_DataModel_Unloads_Old_Model_Before_Loading_New_Model()
    {
        var sourcePath = typeof(ConcurrencyFakeMod).Assembly.Location;
        var tempDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "rml_modloader_replacement_" + Guid.NewGuid());
        var modPath = System.IO.Path.Combine(tempDir, "replacement_mod.dll");
        var lifecycleLog = System.IO.Path.Combine(tempDir, "lifecycle.log");
        System.IO.Directory.CreateDirectory(tempDir);
        System.IO.File.Copy(sourcePath, modPath);
        Environment.SetEnvironmentVariable("RML_TEST_LIFECYCLE_LOG", lifecycleLog);

        try
        {
            ModLoader.LoadMod(modPath);
            ModLoader.OnDataModelChanged(0, 1, DataModelType.Edit);
            ModLoader.OnDataModelChanged(1, 2, DataModelType.Edit);

            Assert.Equal(
                ["load", "data-model-loaded", "data-model-unloaded", "data-model-loaded"],
                System.IO.File.ReadAllLines(lifecycleLog));
        }
        finally
        {
            ModLoader.UnloadMod(modPath);
            Environment.SetEnvironmentVariable("RML_TEST_LIFECYCLE_LOG", null);
            DeleteDirectoryWithRetries(tempDir);
        }
    }

    [Fact]
    public async Task LoadUnload_Interleaved_With_OnDataModelChanged_Does_Not_Throw()
    {
        var sourcePath = typeof(ConcurrencyFakeMod).Assembly.Location;

        var tempDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "rml_modloader_concurrency_" + Guid.NewGuid());
        System.IO.Directory.CreateDirectory(tempDir);

        try
        {
            const int modCount = 6;
            var modPaths = new string[modCount];
            for (var i = 0; i < modCount; i++)
            {
                modPaths[i] = System.IO.Path.Combine(tempDir, $"fake_mod_{i}.dll");
                System.IO.File.Copy(sourcePath, modPaths[i]);
            }

            using var cts = new CancellationTokenSource();
            var exceptions = new System.Collections.Concurrent.ConcurrentBag<Exception>();

            var dataModelTask = Task.Run(() =>
            {
                ulong handle = 1;
                while (!cts.IsCancellationRequested)
                {
                    try
                    {
                        ModLoader.OnDataModelChanged(0, handle, DataModelType.Edit);
                        ModLoader.OnDataModelChanged(handle, 0, DataModelType.Edit);
                    }
                    catch (Exception e)
                    {
                        exceptions.Add(e);
                    }

                    handle++;
                }
            });

            var loadUnloadTasks = modPaths.Select(path => Task.Run(() =>
            {
                for (var i = 0; i < 25; i++)
                {
                    try
                    {
                        ModLoader.LoadMod(path);
                        ModLoader.UnloadMod(path);
                    }
                    catch (Exception e)
                    {
                        exceptions.Add(e);
                    }
                }
            })).ToArray();

            await Task.WhenAll(loadUnloadTasks);
            cts.Cancel();
            await dataModelTask;

            Assert.Empty(exceptions);
        }
        finally
        {
            ModLoader.Shutdown();
            DeleteDirectoryWithRetries(tempDir);
        }
    }

    private static void DeleteDirectoryWithRetries(string path)
    {
        for (var i = 0; i < 10; i++)
        {
            try
            {
                System.IO.Directory.Delete(path, true);
                return;
            }
            catch (System.IO.IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }

            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
    }
}
