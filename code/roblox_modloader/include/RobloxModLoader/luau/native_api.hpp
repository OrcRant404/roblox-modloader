#pragma once

#include <RobloxModLoader/rml_export.hpp>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RML_NATIVE_API_ABI_VERSION 1u
#define RML_NATIVE_API_MAX_EXPORT_SLOTS 64u

typedef struct RmlNativeApiOwner RmlNativeApiOwner;
typedef uint32_t RmlNativeApiExportSlot;

typedef enum RmlNativeApiStatus {
	RML_NATIVE_API_OK = 0,
	RML_NATIVE_API_INVALID_ARGUMENT,
	RML_NATIVE_API_INVALID_NAME,
	RML_NATIVE_API_RESERVED_NAME,
	RML_NATIVE_API_DUPLICATE_NAMESPACE,
	RML_NATIVE_API_DUPLICATE_EXPORT,
	RML_NATIVE_API_INVALID_OWNER,
	RML_NATIVE_API_NAMESPACE_REVOKED,
	RML_NATIVE_API_SLOT_LIMIT_REACHED,
	RML_NATIVE_API_INVALID_SLOT,
	RML_NATIVE_API_REMOVED_EXPORT,
	RML_NATIVE_API_CALLBACK_ERROR,
	RML_NATIVE_API_CALLBACK_EXCEPTION,
	RML_NATIVE_API_ALLOCATION_FAILURE,
	RML_NATIVE_API_REENTRANT_REVOKE,
	RML_NATIVE_API_PROVIDER_IMAGE_UNAVAILABLE,
	RML_NATIVE_API_NAMESPACE_NOT_READY,
	RML_NATIVE_API_NAMESPACE_PUBLISHED
} RmlNativeApiStatus;

typedef enum RmlNativeApiCallbackStatus {
	RML_NATIVE_API_CALLBACK_SUCCESS = 0,
	RML_NATIVE_API_CALLBACK_FAILURE = 1
} RmlNativeApiCallbackStatus;

typedef enum RmlNativeApiExportKind {
	RML_NATIVE_API_STRING_EXPORT = 0,
	RML_NATIVE_API_ENGINE_EXPORT = 1
} RmlNativeApiExportKind;

typedef struct RmlNativeApiOwnedString RmlNativeApiOwnedString;

/* Version 1 verified engine API. States are opaque and valid only during
 * this synchronous dispatch; providers must not retain state or call from Qt.
 * engine_api points to the matching EngineApi from RobloxModLoader/luau/engine_api.hpp. */
typedef struct RmlNativeApiEngineContext {
	uint32_t abi_version;
	void* state;
	const void* engine_api;
} RmlNativeApiEngineContext;

typedef RmlNativeApiCallbackStatus (*RmlNativeApiEngineCallback)(
	void* userdata, const RmlNativeApiEngineContext* context,
	int* result_count, RmlNativeApiOwnedString* error_message);

typedef struct RmlNativeApiOwnedString {
	char* data;
	union {
		size_t size;
		size_t length; // Alias for providers that prefer explicit byte-count naming.
	};
} RmlNativeApiOwnedString;

typedef RmlNativeApiCallbackStatus (*RmlNativeApiStringCallback)(
	void* userdata,
	const char* input,
	size_t input_length,
	RmlNativeApiOwnedString* output,
	RmlNativeApiOwnedString* error_message);

typedef struct RmlNativeApiExportInfo {
	const char* namespace_name;
	const char* function_name;
	RmlNativeApiExportSlot slot;
} RmlNativeApiExportInfo;

typedef struct RmlNativeApiSnapshot {
	RmlNativeApiExportInfo* entries;
	size_t count;
} RmlNativeApiSnapshot;

/*
 * Provider ABI for compiled native mods. Namespace identifiers are lowercase
 * slugs (letters, digits, '_' and '-'); function names are C-style identifiers.
 * The reserved namespaces "ui", "mods", "modloader", and "rml" belong to the
 * loader. Export names beginning with "__" are reserved.
 *
 * A callback must return any output/error strings using
 * rml_native_api_owned_string_assign. Dispatch copies the callback result into
 * loader-owned memory before returning. The caller must destroy every returned
 * owned string. Callbacks receive a length-delimited byte string; embedded NUL
 * bytes are supported. String outputs passed to assign must be zero-initialized
 * and empty; destroy them before assigning a replacement.
 *
 * Export slots are process-lifetime IDs in [1, MAX_EXPORT_SLOTS]. Revocation
 * tombstones slots; IDs are never reused. Snapshot name pointers remain valid
 * until process exit; destroy the snapshot to release its descriptor array.
 */
RML_EXPORT uint32_t rml_native_api_abi_version(void);

/* Register all exports, then publish once the provider is ready. Unpublished
 * namespaces are omitted from snapshots and cannot dispatch. Published export
 * sets are immutable. Revoke before releasing callback userdata. */
RML_EXPORT RmlNativeApiStatus rml_native_api_publish_namespace(
	RmlNativeApiOwner* owner);

RML_EXPORT RmlNativeApiStatus rml_native_api_register_namespace(
	const char* namespace_name,
	RmlNativeApiOwner** out_owner);

RML_EXPORT RmlNativeApiStatus rml_native_api_register_string_export(
	RmlNativeApiOwner* owner,
	const char* function_name,
	RmlNativeApiStringCallback callback,
	void* userdata,
	RmlNativeApiExportSlot* out_slot);

RML_EXPORT RmlNativeApiStatus rml_native_api_register_engine_export(
	RmlNativeApiOwner* owner, const char* function_name,
	RmlNativeApiEngineCallback callback, void* userdata,
	RmlNativeApiExportSlot* out_slot);

RML_EXPORT RmlNativeApiStatus rml_native_api_export_kind(
	RmlNativeApiExportSlot slot, RmlNativeApiExportKind* out_kind);

RML_EXPORT RmlNativeApiStatus rml_native_api_dispatch_engine(
	RmlNativeApiExportSlot slot, const RmlNativeApiEngineContext* context,
	int* result_count, RmlNativeApiOwnedString* error_message);

/* Synchronously disables all exports and waits for in-flight callbacks to end. */
RML_EXPORT RmlNativeApiStatus rml_native_api_revoke_namespace(
	RmlNativeApiOwner* owner);

/* Returns a typed status; callback errors are also returned as owned text. */
RML_EXPORT RmlNativeApiStatus rml_native_api_dispatch_string(
	RmlNativeApiExportSlot slot,
	const char* input,
	size_t input_length,
	RmlNativeApiOwnedString* out_value,
	RmlNativeApiOwnedString* out_error_message);

RML_EXPORT RmlNativeApiStatus rml_native_api_owned_string_assign(
	RmlNativeApiOwnedString* output,
	const char* data,
	size_t length);

RML_EXPORT void rml_native_api_owned_string_destroy(
	RmlNativeApiOwnedString* value);

RML_EXPORT RmlNativeApiStatus rml_native_api_snapshot(
	RmlNativeApiSnapshot* out_snapshot);

RML_EXPORT void rml_native_api_snapshot_destroy(
	RmlNativeApiSnapshot* snapshot);

#ifdef __cplusplus
}

#include <string>
#include <string_view>

inline constexpr uint32_t rml_native_api_max_export_slots =
	RML_NATIVE_API_MAX_EXPORT_SLOTS;

namespace rml::native_api {
	struct OwnedStringGuard {
		RmlNativeApiOwnedString value{};
		~OwnedStringGuard() { rml_native_api_owned_string_destroy(&value); }
	};

	struct DispatchResult {
		RmlNativeApiStatus status{RML_NATIVE_API_OK};
		std::string value;
		std::string error_message;
	};

	inline DispatchResult dispatch_string(const RmlNativeApiExportSlot slot, const std::string_view input)
	{
		OwnedStringGuard value;
		OwnedStringGuard error;
		DispatchResult result;
		result.status = rml_native_api_dispatch_string(slot, input.data(), input.size(), &value.value, &error.value);
		if (value.value.data)
			result.value.assign(value.value.data, value.value.length);
		if (error.value.data)
			result.error_message.assign(error.value.data, error.value.length);
		return result;
	}
}
#endif
