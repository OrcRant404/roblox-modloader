#include "workspace_host.hpp"
#include "workspace_lua.hpp"
#include <RobloxModLoader/mod/mod_base.hpp>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace {
RmlNativeApiOwner* owner{};
bool retain_image() {
    Dl_info info{};
    return dladdr(reinterpret_cast<const void*>(&retain_image),&info) && info.dli_fname
        && dlopen(info.dli_fname,RTLD_NOW|RTLD_LOCAL);
}
class WorkspaceMod final:public ModBase {
public:
    WorkspaceMod(){name="Workspace";version="0.1.0";author="orcrant404";}
    void on_load() override {
        if(rml_native_api_abi_version()!=RML_NATIVE_API_ABI_VERSION) throw std::runtime_error("Workspace requires the matching built-in RML native API");
        const char* verified=std::getenv("VFX_TEST_PROFILE_VERIFIED");
        if(!verified || std::string_view(verified)!="1") throw std::runtime_error("Workspace requires the verified fenced test launcher");
        if(owner) throw std::runtime_error("Workspace hot reload requires a restart");
        if(!retain_image()) throw std::runtime_error("Cannot retain workspace callbacks");
        std::array<char,4096> path{}; std::uint32_t size=path.size();
        if(_NSGetExecutablePath(path.data(),&size)!=0 || std::filesystem::weakly_canonical(path.data())!=std::filesystem::weakly_canonical(RML_TEST_EXECUTABLE))
            throw std::runtime_error("Workspace is restricted to the marked separate Studio");
        rml::workspace::start_host(std::filesystem::path(path.data()).parent_path()/"RobloxModLoader/mods/workspace/reports");
        try {
            if(rml_native_api_register_namespace("workspace",&owner)!=RML_NATIVE_API_OK) throw std::runtime_error("Cannot register workspace namespace");
            rml::workspace::register_lua_exports(owner);
            if(rml_native_api_publish_namespace(owner)!=RML_NATIVE_API_OK) throw std::runtime_error("Cannot publish workspace namespace");
        } catch(...) { if(owner) rml_native_api_revoke_namespace(owner); rml::workspace::stop_host(); throw; }
    }
    void on_unload() override {
        rml::workspace::deactivate_lua_exports();
        if(owner) rml_native_api_revoke_namespace(owner);
        try { rml::workspace::stop_host(); }
        catch(const std::exception& error) { std::fprintf(stderr,"[workspace] unload restoration could not be scheduled: %s\n",error.what()); }
        catch(...) { std::fprintf(stderr,"[workspace] unload restoration could not be scheduled\n"); }
    }
};
}
RML_EXPORT_MOD_ABI_VERSION();
extern "C" RML_MOD_ABI_EXPORT ModBase* start_mod(){return new WorkspaceMod;}
extern "C" RML_MOD_ABI_EXPORT void uninstall_mod(const ModBase* mod){delete mod;}
