#pragma once

// Keep the standalone Core test independent of the loader's logger runtime.
// Arguments are still evaluated, so diagnostics cannot hide compile errors.
namespace rml::test {
template<class... Args>
void discard_log(Args&&...) {}
}

#define RML_LOG_SCOPE(name)
#define RML_INFO(...) ::rml::test::discard_log(__VA_ARGS__)
#define RML_WARN(...) ::rml::test::discard_log(__VA_ARGS__)
