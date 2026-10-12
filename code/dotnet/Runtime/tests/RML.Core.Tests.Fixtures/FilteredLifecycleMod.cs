using RML.Core.Api;
using RML.Core.Modding;
using Roblox;

namespace RML.Core.Tests.Fixtures;

[Mod("rml.tests.filtered-lifecycle", "1.0",
    LoadInDataModels = [DataModelType.Edit, DataModelType.Server, DataModelType.Standalone])]
public sealed class FilteredLifecycleMod : IMod, IDataModelAware
{
    public int OnLoad()
    {
        Record("load");
        return 0;
    }

    public void OnUnload() => Record("unload");

    public void OnDataModelLoaded(DataModel game, DataModelType dataModelType)
        => Record($"loaded:{dataModelType}");

    public void OnDataModelUnloaded(DataModel game, DataModelType dataModelType)
        => Record($"unloaded:{dataModelType}");

    private static void Record(string value)
    {
        var path = Environment.GetEnvironmentVariable("RML_TEST_FILTERED_LIFECYCLE_LOG");
        if (path != null)
            System.IO.File.AppendAllLines(path, [value]);
    }
}
