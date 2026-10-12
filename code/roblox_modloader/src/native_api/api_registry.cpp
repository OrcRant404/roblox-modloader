#include "RobloxModLoader/luau/native_api.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct RmlNativeApiOwner {
	uint64_t marker{};
};

namespace {
	constexpr std::size_t kMaxNameLength = 64;
	thread_local unsigned int g_dispatch_depth = 0;

	struct OwnerRecord;

	struct SlotRecord {
		RmlNativeApiExportSlot slot{};
		std::string namespace_name;
		std::string function_name;
		const OwnerRecord* owner{};
		RmlNativeApiStringCallback callback{};
		RmlNativeApiEngineCallback engine_callback{};
		void* userdata{};

		std::mutex mutex;
		std::condition_variable drained;
		bool active{true};
		std::size_t in_flight{};
	};

	struct OwnerRecord {
		std::unique_ptr<RmlNativeApiOwner> token;
		std::string namespace_name;
		std::unordered_map<std::string, std::shared_ptr<SlotRecord>> exports;
		bool active{true}; // Protected by Registry::mutex.
		bool published{}; // Exports become visible together after publication.

		std::mutex revoke_mutex;
		std::condition_variable revoke_complete_cv;
		bool revoke_complete{};
	};

	struct Registry {
		Registry()
		{
			slots.reserve(RML_NATIVE_API_MAX_EXPORT_SLOTS);
			provider_image_pins.reserve(RML_NATIVE_API_MAX_EXPORT_SLOTS);
		}

		std::mutex mutex;
		std::vector<std::shared_ptr<SlotRecord>> slots;
		std::unordered_map<std::string, std::shared_ptr<OwnerRecord>> active_namespaces;
		// Retain owner records and their opaque tokens for the life of the process.
		std::unordered_map<const RmlNativeApiOwner*, std::shared_ptr<OwnerRecord>> owners;
		// dlopen references are intentionally kept until process exit so stale
		// trampolines cannot point into an unloaded native provider image.
		std::vector<void*> provider_image_pins;
	};

	Registry& registry()
	{
		// Cached closures may run while other static objects are being torn down.
		// Keep registry records, tokens and callback image pins until process exit.
		static auto* instance = new Registry;
		return *instance;
	}

	bool is_namespace_char(const char ch, const bool first)
	{
		return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
		       (!first && (ch == '_' || ch == '-'));
	}

	bool is_function_char(const char ch, const bool first)
	{
		return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		       (!first && ((ch >= '0' && ch <= '9') || ch == '_')) ||
		       (first && ch == '_');
	}

	RmlNativeApiStatus read_name(const char* value, std::string& out, const bool is_namespace)
	{
		if (!value || value[0] == '\0')
			return RML_NATIVE_API_INVALID_NAME;

		std::size_t length = 0;
		while (length <= kMaxNameLength && value[length] != '\0')
			++length;
		if (length == 0 || length > kMaxNameLength)
			return RML_NATIVE_API_INVALID_NAME;

		const std::string_view candidate(value, length);
		for (std::size_t i = 0; i < candidate.size(); ++i)
		{
			const bool valid = is_namespace ? is_namespace_char(candidate[i], i == 0)
			                                : is_function_char(candidate[i], i == 0);
			if (!valid)
				return RML_NATIVE_API_INVALID_NAME;
		}

		if (is_namespace)
		{
			static constexpr std::array<std::string_view, 4> reserved{
				"ui", "mods", "modloader", "rml"
			};
			if (std::ranges::find(reserved, candidate) != reserved.end())
				return RML_NATIVE_API_RESERVED_NAME;
		}
		else if (candidate.starts_with("__"))
		{
			return RML_NATIVE_API_RESERVED_NAME;
		}

		out.assign(candidate);
		return RML_NATIVE_API_OK;
	}

	RmlNativeApiStatus set_error_message(
		RmlNativeApiOwnedString* destination,
		const RmlNativeApiStatus status,
		const char* message)
	{
		if (!destination)
			return status;
		const auto length = std::strlen(message);
		if (rml_native_api_owned_string_assign(destination, message, length) != RML_NATIVE_API_OK)
			return status;
		return status;
	}

	bool pin_provider_image(const void* callback, void** out_handle)
	{
		*out_handle = nullptr;
#if defined(__APPLE__) || defined(__linux__)
		Dl_info callback_image{};
		Dl_info registry_image{};
		if (!dladdr(reinterpret_cast<const void*>(callback), &callback_image) || !callback_image.dli_fname ||
		    !dladdr(reinterpret_cast<const void*>(&rml_native_api_register_string_export), &registry_image) ||
		    !registry_image.dli_fname)
			return false;

		// Tests and statically-linked providers may live in the registry image
		// itself. That image is already pinned by the running process.
		if (std::strcmp(callback_image.dli_fname, registry_image.dli_fname) == 0)
			return true;

		*out_handle = dlopen(callback_image.dli_fname, RTLD_NOW | RTLD_LOCAL);
		return *out_handle != nullptr;
#elif defined(_WIN32)
		HMODULE image{};
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		    reinterpret_cast<LPCSTR>(callback), &image)) return false;
		*out_handle = reinterpret_cast<void*>(image);
		return true;
#else
		(void)callback;
		return false;
#endif
	}

	class InFlightGuard {
	public:
		explicit InFlightGuard(std::shared_ptr<SlotRecord> record) : m_record(std::move(record))
		{
			std::scoped_lock lock(m_record->mutex);
			if (m_record->active)
			{
				++m_record->in_flight;
				m_entered = true;
			}
		}

		~InFlightGuard()
		{
			if (!m_entered)
				return;
			std::scoped_lock lock(m_record->mutex);
			if (--m_record->in_flight == 0)
				m_record->drained.notify_all();
		}

		[[nodiscard]] bool entered() const noexcept { return m_entered; }

	private:
		std::shared_ptr<SlotRecord> m_record;
		bool m_entered{};
	};

	RmlNativeApiStatus dispatch_failure(
		RmlNativeApiStatus status,
		RmlNativeApiOwnedString* out_error,
		const char* message)
	{
		return set_error_message(out_error, status, message);
	}
}

extern "C" RML_EXPORT uint32_t rml_native_api_abi_version(void)
{
	return RML_NATIVE_API_ABI_VERSION;
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_publish_namespace(RmlNativeApiOwner* owner)
{
	if (!owner) return RML_NATIVE_API_INVALID_ARGUMENT;
	try
	{
		auto& state = registry();
		std::scoped_lock lock(state.mutex);
		const auto it = state.owners.find(owner);
		if (it == state.owners.end()) return RML_NATIVE_API_INVALID_OWNER;
		if (!it->second->active) return RML_NATIVE_API_NAMESPACE_REVOKED;
		if (it->second->exports.empty()) return RML_NATIVE_API_NAMESPACE_NOT_READY;
		it->second->published = true;
		return RML_NATIVE_API_OK;
	}
	catch (...) { return RML_NATIVE_API_ALLOCATION_FAILURE; }
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_owned_string_assign(
	RmlNativeApiOwnedString* output,
	const char* data,
	const size_t length)
{
	if (!output || (!data && length != 0) || length == static_cast<size_t>(-1))
		return RML_NATIVE_API_INVALID_ARGUMENT;

	char* copy = static_cast<char*>(std::malloc(length + 1));
	if (!copy)
		return RML_NATIVE_API_ALLOCATION_FAILURE;
	if (length != 0)
		std::memcpy(copy, data, length);
	copy[length] = '\0';
	output->data = copy;
	output->length = length;
	return RML_NATIVE_API_OK;
}

extern "C" RML_EXPORT void rml_native_api_owned_string_destroy(RmlNativeApiOwnedString* value)
{
	if (!value)
		return;
	std::free(value->data);
	value->data = nullptr;
	value->length = 0;
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_register_namespace(
	const char* namespace_name,
	RmlNativeApiOwner** out_owner)
{
	if (!out_owner)
		return RML_NATIVE_API_INVALID_ARGUMENT;
	*out_owner = nullptr;

	try
	{
		std::string name;
		if (const auto status = read_name(namespace_name, name, true); status != RML_NATIVE_API_OK)
			return status;

		auto& state = registry();
		std::scoped_lock lock(state.mutex);
		if (state.active_namespaces.contains(name))
			return RML_NATIVE_API_DUPLICATE_NAMESPACE;

		auto record = std::make_shared<OwnerRecord>();
		record->token = std::make_unique<RmlNativeApiOwner>();
		record->token->marker = static_cast<uint64_t>(state.owners.size() + 1);
		record->namespace_name = name;
		record->exports.reserve(RML_NATIVE_API_MAX_EXPORT_SLOTS);
		const auto* token = record->token.get();

		const auto [owner_it, owner_inserted] = state.owners.emplace(token, record);
		if (!owner_inserted)
			return RML_NATIVE_API_ALLOCATION_FAILURE;
		try
		{
			state.active_namespaces.emplace(name, record);
		}
		catch (...)
		{
			state.owners.erase(owner_it);
			throw;
		}

		*out_owner = record->token.get();
		return RML_NATIVE_API_OK;
	}
	catch (...)
	{
		return RML_NATIVE_API_ALLOCATION_FAILURE;
	}
}

static RmlNativeApiStatus register_export(
	RmlNativeApiOwner* owner,
	const char* function_name,
	RmlNativeApiStringCallback callback,
	RmlNativeApiEngineCallback engine_callback,
	void* userdata,
	RmlNativeApiExportSlot* out_slot)
{
	void* image_pin = nullptr;
	if (out_slot)
		*out_slot = 0;
	if (!owner || (!callback && !engine_callback) || !out_slot)
		return RML_NATIVE_API_INVALID_ARGUMENT;

	try
	{
		std::string name;
		if (const auto status = read_name(function_name, name, false); status != RML_NATIVE_API_OK)
			return status;

		auto& state = registry();
		std::scoped_lock lock(state.mutex);
		const auto owner_it = state.owners.find(owner);
		if (owner_it == state.owners.end())
			return RML_NATIVE_API_INVALID_OWNER;
		const auto& owner_record = owner_it->second;
		if (!owner_record->active)
			return RML_NATIVE_API_NAMESPACE_REVOKED;
		if (owner_record->published)
			return RML_NATIVE_API_NAMESPACE_PUBLISHED;
		if (owner_record->exports.contains(name))
			return RML_NATIVE_API_DUPLICATE_EXPORT;
		if (state.slots.size() >= RML_NATIVE_API_MAX_EXPORT_SLOTS)
			return RML_NATIVE_API_SLOT_LIMIT_REACHED;
		const auto* function = callback ? reinterpret_cast<const void*>(callback) : reinterpret_cast<const void*>(engine_callback);
		if (!pin_provider_image(function, &image_pin))
			return RML_NATIVE_API_PROVIDER_IMAGE_UNAVAILABLE;

		auto slot_record = std::make_shared<SlotRecord>();
		slot_record->slot = static_cast<RmlNativeApiExportSlot>(state.slots.size() + 1);
		slot_record->namespace_name = owner_record->namespace_name;
		slot_record->function_name = name;
		slot_record->owner = owner_record.get();
		slot_record->callback = callback;
		slot_record->engine_callback = engine_callback;
		slot_record->userdata = userdata;

		owner_record->exports.emplace(name, slot_record);
		state.slots.push_back(slot_record); // Capacity was reserved up front.
		if (image_pin)
		{
			state.provider_image_pins.push_back(image_pin); // Capacity was reserved up front.
			image_pin = nullptr;
		}
		*out_slot = slot_record->slot;
		return RML_NATIVE_API_OK;
	}
	catch (...)
	{
		if (image_pin)
#if defined(_WIN32)
			FreeLibrary(reinterpret_cast<HMODULE>(image_pin));
#else
			dlclose(image_pin);
#endif
		return RML_NATIVE_API_ALLOCATION_FAILURE;
	}
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_register_string_export(
	RmlNativeApiOwner* owner, const char* name, RmlNativeApiStringCallback callback,
	void* userdata, RmlNativeApiExportSlot* slot)
{
	return register_export(owner, name, callback, nullptr, userdata, slot);
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_register_engine_export(
	RmlNativeApiOwner* owner, const char* name, RmlNativeApiEngineCallback callback,
	void* userdata, RmlNativeApiExportSlot* slot)
{
	return register_export(owner, name, nullptr, callback, userdata, slot);
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_export_kind(
	RmlNativeApiExportSlot slot, RmlNativeApiExportKind* kind)
{
	if (!kind) return RML_NATIVE_API_INVALID_ARGUMENT;
	auto& state = registry();
	std::scoped_lock lock(state.mutex);
	if (!slot || slot > state.slots.size()) return RML_NATIVE_API_INVALID_SLOT;
	*kind = state.slots[slot-1]->engine_callback ? RML_NATIVE_API_ENGINE_EXPORT : RML_NATIVE_API_STRING_EXPORT;
	return RML_NATIVE_API_OK;
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_dispatch_engine(
	RmlNativeApiExportSlot slot, const RmlNativeApiEngineContext* context,
	int* count, RmlNativeApiOwnedString* error)
{
	if (count) *count = 0;
	if (error) *error = {};
	if (!context || context->abi_version != RML_NATIVE_API_ABI_VERSION || !context->state || !context->engine_api || !count || !error)
		return RML_NATIVE_API_INVALID_ARGUMENT;
	try {
		std::shared_ptr<SlotRecord> record;
		{ auto& state = registry(); std::scoped_lock lock(state.mutex);
		  if (!slot || slot > state.slots.size()) return dispatch_failure(RML_NATIVE_API_INVALID_SLOT, error, "unknown export slot");
		  record = state.slots[slot-1];
		  if (!record->owner->published) return dispatch_failure(RML_NATIVE_API_NAMESPACE_NOT_READY, error, "native namespace is not ready"); }
		InFlightGuard in_flight(record);
		if (!in_flight.entered()) return dispatch_failure(RML_NATIVE_API_REMOVED_EXPORT, error, "export has been removed");
		if (!record->engine_callback) return dispatch_failure(RML_NATIVE_API_INVALID_ARGUMENT, error, "export expects string dispatch");
		RmlNativeApiCallbackStatus status{};
		try { ++g_dispatch_depth; status = record->engine_callback(record->userdata, context, count, error); --g_dispatch_depth; }
		catch (const std::exception& exception) { --g_dispatch_depth; rml_native_api_owned_string_destroy(error); return dispatch_failure(RML_NATIVE_API_CALLBACK_EXCEPTION, error, exception.what()); }
		catch (...) { --g_dispatch_depth; rml_native_api_owned_string_destroy(error); return dispatch_failure(RML_NATIVE_API_CALLBACK_EXCEPTION, error, "engine provider callback threw an exception"); }
		if (status == RML_NATIVE_API_CALLBACK_SUCCESS && *count >= 0 && *count <= 64) return RML_NATIVE_API_OK;
		*count = 0;
		if (!error->data) return dispatch_failure(RML_NATIVE_API_CALLBACK_ERROR, error, "engine provider callback failed");
		return RML_NATIVE_API_CALLBACK_ERROR;
	} catch (...) { return dispatch_failure(RML_NATIVE_API_ALLOCATION_FAILURE, error, "native API registry allocation failed"); }
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_revoke_namespace(RmlNativeApiOwner* owner)
{
	if (!owner)
		return RML_NATIVE_API_INVALID_ARGUMENT;
	if (g_dispatch_depth != 0)
		return RML_NATIVE_API_REENTRANT_REVOKE;

	try
	{
		auto& state = registry();
		std::shared_ptr<OwnerRecord> owner_record;
		bool begin_revoke = false;
		{
			std::scoped_lock lock(state.mutex);
			const auto it = state.owners.find(owner);
			if (it == state.owners.end())
				return RML_NATIVE_API_INVALID_OWNER;
			owner_record = it->second;
			if (owner_record->active)
			{
				owner_record->active = false;
				const auto namespace_it = state.active_namespaces.find(owner_record->namespace_name);
				if (namespace_it != state.active_namespaces.end() && namespace_it->second == owner_record)
					state.active_namespaces.erase(namespace_it);
				begin_revoke = true;
			}
		}

		if (!begin_revoke)
		{
			std::unique_lock lock(owner_record->revoke_mutex);
			owner_record->revoke_complete_cv.wait(lock, [&] { return owner_record->revoke_complete; });
			return RML_NATIVE_API_NAMESPACE_REVOKED;
		}

		// First close every slot to new dispatches, then drain already-running calls.
		for (const auto& [name, slot] : owner_record->exports)
		{
			(void)name;
			std::scoped_lock lock(slot->mutex);
			slot->active = false;
		}
		for (const auto& [name, slot] : owner_record->exports)
		{
			(void)name;
			std::unique_lock lock(slot->mutex);
			slot->drained.wait(lock, [&] { return slot->in_flight == 0; });
		}

		{
			std::scoped_lock lock(owner_record->revoke_mutex);
			owner_record->revoke_complete = true;
		}
		owner_record->revoke_complete_cv.notify_all();
		return RML_NATIVE_API_OK;
	}
	catch (...)
	{
		return RML_NATIVE_API_ALLOCATION_FAILURE;
	}
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_dispatch_string(
	const RmlNativeApiExportSlot slot,
	const char* input,
	const size_t input_length,
	RmlNativeApiOwnedString* out_value,
	RmlNativeApiOwnedString* out_error_message)
{
	if (out_value)
		*out_value = {};
	if (out_error_message)
		*out_error_message = {};
	if (!out_value || !out_error_message || (!input && input_length != 0))
		return RML_NATIVE_API_INVALID_ARGUMENT;

	try
	{
		std::shared_ptr<SlotRecord> slot_record;
		{
			auto& state = registry();
			std::scoped_lock lock(state.mutex);
			if (slot == 0 || slot > state.slots.size())
				return dispatch_failure(RML_NATIVE_API_INVALID_SLOT, out_error_message, "unknown export slot");
			slot_record = state.slots[slot - 1];
			if (!slot_record->owner->published)
				return dispatch_failure(RML_NATIVE_API_NAMESPACE_NOT_READY, out_error_message, "native namespace is not ready");
		}

		InFlightGuard in_flight(slot_record);
		if (!in_flight.entered())
			return dispatch_failure(RML_NATIVE_API_REMOVED_EXPORT, out_error_message, "export has been removed");
		if (!slot_record->callback)
			return dispatch_failure(RML_NATIVE_API_INVALID_ARGUMENT, out_error_message, "export expects engine dispatch");

		RmlNativeApiCallbackStatus callback_status = RML_NATIVE_API_CALLBACK_FAILURE;
		RmlNativeApiOwnedString callback_value{};
		RmlNativeApiOwnedString callback_error{};
		try
		{
			++g_dispatch_depth;
			callback_status = slot_record->callback(
				slot_record->userdata,
				input,
				input_length,
				&callback_value,
				&callback_error);
			--g_dispatch_depth;
		}
		catch (const std::exception& exception)
		{
			--g_dispatch_depth;
			rml_native_api_owned_string_destroy(&callback_value);
			rml_native_api_owned_string_destroy(&callback_error);
			return dispatch_failure(RML_NATIVE_API_CALLBACK_EXCEPTION, out_error_message, exception.what());
		}
		catch (...)
		{
			--g_dispatch_depth;
			rml_native_api_owned_string_destroy(&callback_value);
			rml_native_api_owned_string_destroy(&callback_error);
			return dispatch_failure(RML_NATIVE_API_CALLBACK_EXCEPTION, out_error_message, "provider callback threw an unknown exception");
		}

		if (callback_status == RML_NATIVE_API_CALLBACK_SUCCESS)
		{
			rml_native_api_owned_string_destroy(&callback_error);
			*out_value = callback_value;
			return RML_NATIVE_API_OK;
		}

		rml_native_api_owned_string_destroy(&callback_value);
		if (callback_status != RML_NATIVE_API_CALLBACK_FAILURE)
		{
			rml_native_api_owned_string_destroy(&callback_error);
			return dispatch_failure(RML_NATIVE_API_CALLBACK_ERROR, out_error_message, "provider callback returned an invalid status");
		}

		if (!callback_error.data)
			(void)rml_native_api_owned_string_assign(&callback_error, "provider callback failed", 24);
		*out_error_message = callback_error;
		return RML_NATIVE_API_CALLBACK_ERROR;
	}
	catch (...)
	{
		return dispatch_failure(RML_NATIVE_API_ALLOCATION_FAILURE, out_error_message, "native API registry allocation failed");
	}
}

extern "C" RML_EXPORT RmlNativeApiStatus rml_native_api_snapshot(
	RmlNativeApiSnapshot* out_snapshot)
{
	if (!out_snapshot)
		return RML_NATIVE_API_INVALID_ARGUMENT;
	*out_snapshot = {};

	try
	{
		auto& state = registry();
		std::scoped_lock lock(state.mutex);
		std::size_t count = 0;
		for (const auto& slot : state.slots)
		{
			const auto owner_it = state.active_namespaces.find(slot->namespace_name);
			if (owner_it != state.active_namespaces.end() && owner_it->second->active && owner_it->second->published &&
			    owner_it->second.get() == slot->owner)
				++count;
		}
		if (count == 0)
			return RML_NATIVE_API_OK;

		auto* entries = new (std::nothrow) RmlNativeApiExportInfo[count];
		if (!entries)
			return RML_NATIVE_API_ALLOCATION_FAILURE;

		std::size_t index = 0;
		for (const auto& slot : state.slots)
		{
			const auto owner_it = state.active_namespaces.find(slot->namespace_name);
			if (owner_it == state.active_namespaces.end() || !owner_it->second->active || !owner_it->second->published ||
			    owner_it->second.get() != slot->owner)
				continue;
			entries[index++] = RmlNativeApiExportInfo{
				slot->namespace_name.c_str(),
				slot->function_name.c_str(),
				slot->slot
			};
		}
		out_snapshot->entries = entries;
		out_snapshot->count = count;
		return RML_NATIVE_API_OK;
	}
	catch (...)
	{
		return RML_NATIVE_API_ALLOCATION_FAILURE;
	}
}

extern "C" RML_EXPORT void rml_native_api_snapshot_destroy(RmlNativeApiSnapshot* snapshot)
{
	if (!snapshot)
		return;
	delete[] snapshot->entries;
	*snapshot = {};
}
