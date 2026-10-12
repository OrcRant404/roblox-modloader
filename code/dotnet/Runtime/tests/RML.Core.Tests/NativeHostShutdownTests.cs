using System.Reflection;
using System.Runtime.Loader;
using Xunit;

namespace RML.Core.Tests;

// Exercise the actual unmanaged boundary: an escaping exception here would
// abort the test host, just as it aborted Studio during Qt's aboutToQuit signal.
public sealed class NativeHostShutdownTests
{
    private static readonly Type Host = Assembly.Load("RML.NativeHost")
        .GetType("RML.NativeHost.NativeHost", throwOnError: true)!;

    [Fact]
    public void Shutdown_Keeps_Noncollectible_Core_Alive_And_Is_Idempotent()
    {
        var calls = 0;
        var context = CreateCoreContext();
        var unloading = false;
        context.Unloading += _ => unloading = true;
        Set("_coreAssemblyLoadContext", context);
        Set("_coreAssembly", Host.Assembly);
        Set("_loadModBinding", (Action<string>)(_ => { }));
        Set("_unloadModBinding", (Action<string>)(_ => { }));
        Set("_notifyDataModelChangedBinding", (Action<ulong, ulong, int>)((_, _, _) => { }));
        Set("_shutdownBinding", (Action)(() => { calls++; CallShutdown(); }));

        CallShutdown();
        CallShutdown();

        Assert.Equal(1, calls);
        Assert.False(context.IsCollectible);
        Assert.False(unloading);
        foreach (var field in new[] { "_coreAssemblyLoadContext", "_coreAssembly", "_loadModBinding",
                     "_unloadModBinding", "_notifyDataModelChangedBinding", "_shutdownBinding" })
            Assert.Null(Get(field));
    }

    [Fact]
    public void Shutdown_Contains_Mod_Cleanup_Exception_At_Native_Boundary()
    {
        Set("_coreAssemblyLoadContext", CreateCoreContext());
        Set("_shutdownBinding", (Action)(() => throw new InvalidOperationException("test cleanup failure")));
        CallShutdown();
        Assert.Null(Get("_shutdownBinding"));
        Assert.Null(Get("_coreAssemblyLoadContext"));
        CallShutdown();
    }

    [Fact]
    public void Shutdown_Before_Initialization_Is_Harmless()
    {
        Set("_shutdownBinding", null);
        Set("_coreAssemblyLoadContext", null);
        CallShutdown();
    }

    private static AssemblyLoadContext CreateCoreContext() => (AssemblyLoadContext)Activator.CreateInstance(
        Host.GetNestedType("CoreLoadContext", BindingFlags.NonPublic)!, [Host.Assembly.Location])!;

    private static FieldInfo Field(string name) => Host.GetField(name, BindingFlags.NonPublic | BindingFlags.Static)!;
    private static void Set(string name, object? value) => Field(name).SetValue(null, value);
    private static object? Get(string name) => Field(name).GetValue(null);

    private static unsafe void CallShutdown()
    {
        var entry = (delegate* unmanaged[Cdecl]<void>)Host.GetMethod("Shutdown")!.MethodHandle.GetFunctionPointer();
        entry();
    }
}
