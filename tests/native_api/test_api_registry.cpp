#include <RobloxModLoader/luau/native_api.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
	[[noreturn]] void fail(const char* expression, const int line)
	{
		std::cerr << "FAIL line " << line << ": " << expression << '\n';
		std::exit(1);
	}

#define CHECK(expression) ((expression) ? static_cast<void>(0) : fail(#expression, __LINE__))

	RmlNativeApiOwner* g_reentrant_owner{};
	std::atomic<RmlNativeApiStatus> g_reentrant_status{RML_NATIVE_API_OK};

	RmlNativeApiCallbackStatus echo(
		void*, const char* input, const size_t input_size,
		RmlNativeApiOwnedString* output, RmlNativeApiOwnedString*)
	{
		std::string response = "native-pong:";
		if (input_size != 0)
			response.append(input, input_size);
		return rml_native_api_owned_string_assign(output, response.data(), response.size()) == RML_NATIVE_API_OK
			? RML_NATIVE_API_CALLBACK_SUCCESS
			: RML_NATIVE_API_CALLBACK_FAILURE;
	}

	struct FakeEngineState {
		uint64_t marker{};
	};

	struct FakeEngineApi {
		uint64_t marker{};
	};

	struct FakeEngineProvider {
		FakeEngineState* expected_state{};
		FakeEngineApi* expected_api{};
		int calls{};
	};

	void check_fake_engine_context(void* userdata, const RmlNativeApiEngineContext* context)
	{
		auto& provider = *static_cast<FakeEngineProvider*>(userdata);
		CHECK(context != nullptr);
		CHECK(context->abi_version == RML_NATIVE_API_ABI_VERSION);
		CHECK(context->state == provider.expected_state);
		CHECK(context->engine_api == provider.expected_api);
		CHECK(static_cast<FakeEngineState*>(context->state)->marker == 0x51a7e);
		CHECK(static_cast<const FakeEngineApi*>(context->engine_api)->marker == 0xa91);
		++provider.calls;
	}

	RmlNativeApiCallbackStatus engine_probe(
		void* userdata, const RmlNativeApiEngineContext* context,
		int* result_count, RmlNativeApiOwnedString*)
	{
		check_fake_engine_context(userdata, context);
		*result_count = 3;
		return RML_NATIVE_API_CALLBACK_SUCCESS;
	}

	RmlNativeApiCallbackStatus engine_error(
		void* userdata, const RmlNativeApiEngineContext* context,
		int* result_count, RmlNativeApiOwnedString* error)
	{
		check_fake_engine_context(userdata, context);
		*result_count = 9; // Registry must discard a provider's count on failure.
		constexpr std::string_view message = "expected engine provider error";
		(void)rml_native_api_owned_string_assign(error, message.data(), message.size());
		return RML_NATIVE_API_CALLBACK_FAILURE;
	}

	RmlNativeApiCallbackStatus engine_exception(
		void* userdata, const RmlNativeApiEngineContext* context,
		int*, RmlNativeApiOwnedString*)
	{
		check_fake_engine_context(userdata, context);
		throw std::runtime_error("expected engine callback exception");
	}

	RmlNativeApiCallbackStatus fail_with_message(
		void*, const char*, size_t,
		RmlNativeApiOwnedString*, RmlNativeApiOwnedString* error)
	{
		constexpr std::string_view message = "expected provider error";
		(void)rml_native_api_owned_string_assign(error, message.data(), message.size());
		return RML_NATIVE_API_CALLBACK_FAILURE;
	}

	RmlNativeApiCallbackStatus throw_exception(void*, const char*, size_t,
		RmlNativeApiOwnedString*, RmlNativeApiOwnedString*)
	{
		throw std::runtime_error("expected callback exception");
	}

	RmlNativeApiCallbackStatus throw_unknown_exception(void*, const char*, size_t,
		RmlNativeApiOwnedString*, RmlNativeApiOwnedString*)
	{
		throw 17;
	}

	RmlNativeApiCallbackStatus attempt_reentrant_revoke(
		void*, const char*, size_t,
		RmlNativeApiOwnedString* output, RmlNativeApiOwnedString*)
	{
		g_reentrant_status.store(rml_native_api_revoke_namespace(g_reentrant_owner));
		constexpr std::string_view response = "still-active";
		(void)rml_native_api_owned_string_assign(output, response.data(), response.size());
		return RML_NATIVE_API_CALLBACK_SUCCESS;
	}

	struct BlockContext {
		std::mutex mutex;
		std::condition_variable changed;
		bool entered{};
		bool release{};
		std::atomic_bool first_call{true};
	};

	RmlNativeApiCallbackStatus blocking_callback(
		void* userdata, const char*, size_t,
		RmlNativeApiOwnedString* output, RmlNativeApiOwnedString*)
	{
		auto& context = *static_cast<BlockContext*>(userdata);
		if (context.first_call.exchange(false))
		{
			std::unique_lock lock(context.mutex);
			context.entered = true;
			context.changed.notify_all();
			context.changed.wait(lock, [&] { return context.release; });
		}
		constexpr std::string_view response = "released";
		(void)rml_native_api_owned_string_assign(output, response.data(), response.size());
		return RML_NATIVE_API_CALLBACK_SUCCESS;
	}

	RmlNativeApiOwner* register_namespace(const std::string& name)
	{
		RmlNativeApiOwner* owner{};
		CHECK(rml_native_api_register_namespace(name.c_str(), &owner) == RML_NATIVE_API_OK);
		CHECK(owner != nullptr);
		return owner;
	}

	RmlNativeApiExportSlot register_export(
		RmlNativeApiOwner* owner,
		const char* name,
		RmlNativeApiStringCallback callback,
		void* userdata = nullptr)
	{
		RmlNativeApiExportSlot slot{};
		CHECK(rml_native_api_register_string_export(owner, name, callback, userdata, &slot) == RML_NATIVE_API_OK);
		CHECK(slot >= 1 && slot <= rml_native_api_max_export_slots);
		return slot;
	}

	RmlNativeApiExportSlot register_engine_export(
		RmlNativeApiOwner* owner,
		const char* name,
		RmlNativeApiEngineCallback callback,
		void* userdata)
	{
		RmlNativeApiExportSlot slot{};
		CHECK(rml_native_api_register_engine_export(owner, name, callback, userdata, &slot) == RML_NATIVE_API_OK);
		CHECK(slot >= 1 && slot <= rml_native_api_max_export_slots);
		return slot;
	}

	void test_names_and_collisions()
	{
		CHECK(rml_native_api_abi_version() == RML_NATIVE_API_ABI_VERSION);

		RmlNativeApiOwner* owner{};
		CHECK(rml_native_api_register_namespace(nullptr, &owner) == RML_NATIVE_API_INVALID_NAME);
		CHECK(rml_native_api_register_namespace("", &owner) == RML_NATIVE_API_INVALID_NAME);
		CHECK(rml_native_api_register_namespace("Bad Namespace", &owner) == RML_NATIVE_API_INVALID_NAME);
		CHECK(rml_native_api_register_namespace("ui", &owner) == RML_NATIVE_API_RESERVED_NAME);
		CHECK(rml_native_api_register_namespace("mods", &owner) == RML_NATIVE_API_RESERVED_NAME);
		CHECK(rml_native_api_register_namespace("modloader", &owner) == RML_NATIVE_API_RESERVED_NAME);
		CHECK(rml_native_api_register_namespace("rml", &owner) == RML_NATIVE_API_RESERVED_NAME);
		CHECK(rml_native_api_register_namespace("registry-test", &owner) == RML_NATIVE_API_OK);
		CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_NAMESPACE_NOT_READY);
		RmlNativeApiOwner* duplicate{};
		CHECK(rml_native_api_register_namespace("registry-test", &duplicate) == RML_NATIVE_API_DUPLICATE_NAMESPACE);
		CHECK(duplicate == nullptr);

		RmlNativeApiExportSlot slot{};
		CHECK(rml_native_api_register_string_export(owner, "bad name", echo, nullptr, &slot) == RML_NATIVE_API_INVALID_NAME);
		CHECK(rml_native_api_register_string_export(owner, "__private", echo, nullptr, &slot) == RML_NATIVE_API_RESERVED_NAME);
		CHECK(rml_native_api_register_string_export(owner, "Valid", nullptr, nullptr, &slot) == RML_NATIVE_API_INVALID_ARGUMENT);
		CHECK(rml_native_api_register_string_export(
			reinterpret_cast<RmlNativeApiOwner*>(static_cast<uintptr_t>(1)), "Valid", echo, nullptr, &slot) == RML_NATIVE_API_INVALID_OWNER);
		const auto echo_slot = register_export(owner, "Ping", echo);
		CHECK(rml_native_api_register_string_export(owner, "Ping", echo, nullptr, &slot) == RML_NATIVE_API_DUPLICATE_EXPORT);

		// A provider can register callbacks before it is visible to Luau. Until
		// publish, dispatch must fail closed and snapshots must omit its exports.
		auto unpublished_result = rml::native_api::dispatch_string(echo_slot, "hello");
		CHECK(unpublished_result.status == RML_NATIVE_API_NAMESPACE_NOT_READY);
		CHECK(unpublished_result.error_message == "native namespace is not ready");
		RmlNativeApiSnapshot unpublished_snapshot{};
		CHECK(rml_native_api_snapshot(&unpublished_snapshot) == RML_NATIVE_API_OK);
		CHECK(unpublished_snapshot.count == 0);
		rml_native_api_snapshot_destroy(&unpublished_snapshot);

		CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_OK);
		CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_OK);
		RmlNativeApiExportSlot late_slot{};
		CHECK(rml_native_api_register_string_export(owner, "Late", echo, nullptr, &late_slot)
			== RML_NATIVE_API_NAMESPACE_PUBLISHED);
		CHECK(late_slot == 0);

		auto result = rml::native_api::dispatch_string(echo_slot, "hello");
		CHECK(result.status == RML_NATIVE_API_OK);
		CHECK(result.value == "native-pong:hello");
		RmlNativeApiOwnedString invalid_value{};
		RmlNativeApiOwnedString invalid_error{};
		CHECK(rml_native_api_dispatch_string(echo_slot, nullptr, 1, &invalid_value, &invalid_error)
			== RML_NATIVE_API_INVALID_ARGUMENT);
		rml_native_api_owned_string_destroy(&invalid_value);
		rml_native_api_owned_string_destroy(&invalid_error);

		RmlNativeApiSnapshot snapshot{};
		CHECK(rml_native_api_snapshot(&snapshot) == RML_NATIVE_API_OK);
		CHECK(snapshot.count == 1);
		CHECK(std::string_view(snapshot.entries[0].namespace_name) == "registry-test");
		CHECK(std::string_view(snapshot.entries[0].function_name) == "Ping");
		CHECK(snapshot.entries[0].slot == echo_slot);
		rml_native_api_snapshot_destroy(&snapshot);

		CHECK(rml_native_api_revoke_namespace(owner) == RML_NATIVE_API_OK);
		CHECK(rml_native_api_revoke_namespace(owner) == RML_NATIVE_API_NAMESPACE_REVOKED);
		CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_NAMESPACE_REVOKED);
		result = rml::native_api::dispatch_string(echo_slot, "cached");
		CHECK(result.status == RML_NATIVE_API_REMOVED_EXPORT);

		RmlNativeApiSnapshot empty_snapshot{};
		CHECK(rml_native_api_snapshot(&empty_snapshot) == RML_NATIVE_API_OK);
		CHECK(empty_snapshot.count == 0);
		rml_native_api_snapshot_destroy(&empty_snapshot);

		auto* replacement_owner = register_namespace("registry-test");
		const auto replacement_slot = register_export(replacement_owner, "Ping", echo);
		CHECK(replacement_slot > echo_slot); // Removed slots are tombstoned, never recycled.
		CHECK(rml_native_api_publish_namespace(replacement_owner) == RML_NATIVE_API_OK);
		CHECK(rml_native_api_revoke_namespace(replacement_owner) == RML_NATIVE_API_OK);
	}

	void test_callback_failures()
	{
		auto* owner = register_namespace("errors");
		const auto failure_slot = register_export(owner, "Failure", fail_with_message);
		const auto exception_slot = register_export(owner, "Exception", throw_exception);
		const auto unknown_exception_slot = register_export(owner, "UnknownException", throw_unknown_exception);
		const auto reentrant_slot = register_export(owner, "Revoke", attempt_reentrant_revoke);
		CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_OK);

		auto result = rml::native_api::dispatch_string(failure_slot, "");
		CHECK(result.status == RML_NATIVE_API_CALLBACK_ERROR);
		CHECK(result.error_message == "expected provider error");

		result = rml::native_api::dispatch_string(exception_slot, "");
		CHECK(result.status == RML_NATIVE_API_CALLBACK_EXCEPTION);
		CHECK(result.error_message == "expected callback exception");
		result = rml::native_api::dispatch_string(unknown_exception_slot, "");
		CHECK(result.status == RML_NATIVE_API_CALLBACK_EXCEPTION);
		CHECK(result.error_message == "provider callback threw an unknown exception");

		g_reentrant_owner = owner;
		result = rml::native_api::dispatch_string(reentrant_slot, "");
		CHECK(result.status == RML_NATIVE_API_OK);
		CHECK(result.value == "still-active");
		CHECK(g_reentrant_status.load() == RML_NATIVE_API_REENTRANT_REVOKE);
		CHECK(rml_native_api_revoke_namespace(owner) == RML_NATIVE_API_OK);
	}

	void test_engine_exports()
	{
		FakeEngineState state{0x51a7e};
		FakeEngineApi api{0xa91};
		FakeEngineProvider provider{&state, &api, 0};
		auto* owner = register_namespace("engine-probe");
		const auto probe_slot = register_engine_export(owner, "Probe", engine_probe, &provider);
		const auto error_slot = register_engine_export(owner, "Failure", engine_error, &provider);
		const auto exception_slot = register_engine_export(owner, "Exception", engine_exception, &provider);
		const auto string_slot = register_export(owner, "Echo", echo);
		CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_OK);

		RmlNativeApiExportKind kind{};
		CHECK(rml_native_api_export_kind(probe_slot, &kind) == RML_NATIVE_API_OK);
		CHECK(kind == RML_NATIVE_API_ENGINE_EXPORT);
		CHECK(rml_native_api_export_kind(error_slot, &kind) == RML_NATIVE_API_OK);
		CHECK(kind == RML_NATIVE_API_ENGINE_EXPORT);
		CHECK(rml_native_api_export_kind(string_slot, &kind) == RML_NATIVE_API_OK);
		CHECK(kind == RML_NATIVE_API_STRING_EXPORT);
		CHECK(rml_native_api_export_kind(0, &kind) == RML_NATIVE_API_INVALID_SLOT);
		CHECK(rml_native_api_export_kind(probe_slot, nullptr) == RML_NATIVE_API_INVALID_ARGUMENT);

		// Engine exports must reject the string-only dispatch path without calling
		// their provider or attempting any Lua operation.
		auto string_result = rml::native_api::dispatch_string(probe_slot, "not-an-engine-context");
		CHECK(string_result.status == RML_NATIVE_API_INVALID_ARGUMENT);
		CHECK(string_result.error_message == "export expects engine dispatch");
		CHECK(provider.calls == 0);
		string_result = rml::native_api::dispatch_string(string_slot, "ordinary");
		CHECK(string_result.status == RML_NATIVE_API_OK);
		CHECK(string_result.value == "native-pong:ordinary");

		const RmlNativeApiEngineContext context{RML_NATIVE_API_ABI_VERSION, &state, &api};
		int result_count = -1;
		RmlNativeApiOwnedString error{};
		CHECK(rml_native_api_dispatch_engine(probe_slot, &context, &result_count, &error) == RML_NATIVE_API_OK);
		CHECK(result_count == 3);
		CHECK(error.data == nullptr && error.size == 0);
		CHECK(provider.calls == 1);
		rml_native_api_owned_string_destroy(&error);
		result_count = -1;
		CHECK(rml_native_api_dispatch_engine(string_slot, &context, &result_count, &error) == RML_NATIVE_API_INVALID_ARGUMENT);
		CHECK(result_count == 0);
		CHECK(std::string_view(error.data, error.size) == "export expects string dispatch");
		CHECK(provider.calls == 1);
		rml_native_api_owned_string_destroy(&error);

		result_count = -1;
		CHECK(rml_native_api_dispatch_engine(error_slot, &context, &result_count, &error) == RML_NATIVE_API_CALLBACK_ERROR);
		CHECK(result_count == 0);
		CHECK(std::string_view(error.data, error.size) == "expected engine provider error");
		CHECK(provider.calls == 2);
		rml_native_api_owned_string_destroy(&error);

		result_count = -1;
		CHECK(rml_native_api_dispatch_engine(exception_slot, &context, &result_count, &error) == RML_NATIVE_API_CALLBACK_EXCEPTION);
		CHECK(result_count == 0);
		CHECK(std::string_view(error.data, error.size) == "expected engine callback exception");
		CHECK(provider.calls == 3);
		rml_native_api_owned_string_destroy(&error);

		RmlNativeApiEngineContext invalid_context{RML_NATIVE_API_ABI_VERSION + 1, &state, &api};
		result_count = -1;
		CHECK(rml_native_api_dispatch_engine(probe_slot, &invalid_context, &result_count, &error) == RML_NATIVE_API_INVALID_ARGUMENT);
		CHECK(result_count == 0);
		CHECK(error.data == nullptr && error.size == 0);
		CHECK(provider.calls == 3);

		RmlNativeApiSnapshot snapshot{};
		CHECK(rml_native_api_snapshot(&snapshot) == RML_NATIVE_API_OK);
		CHECK(snapshot.count == 4);
		bool found_probe = false;
		for (size_t index = 0; index < snapshot.count; ++index) {
			if (snapshot.entries[index].slot == probe_slot) {
				CHECK(std::string_view(snapshot.entries[index].namespace_name) == "engine-probe");
				CHECK(std::string_view(snapshot.entries[index].function_name) == "Probe");
				found_probe = true;
			}
		}
		CHECK(found_probe);
		rml_native_api_snapshot_destroy(&snapshot);

		CHECK(rml_native_api_revoke_namespace(owner) == RML_NATIVE_API_OK);
		result_count = -1;
		CHECK(rml_native_api_dispatch_engine(probe_slot, &context, &result_count, &error) == RML_NATIVE_API_REMOVED_EXPORT);
		CHECK(result_count == 0);
		CHECK(std::string_view(error.data, error.size) == "export has been removed");
		CHECK(provider.calls == 3);
		rml_native_api_owned_string_destroy(&error);
		CHECK(rml_native_api_export_kind(probe_slot, &kind) == RML_NATIVE_API_OK);
		CHECK(kind == RML_NATIVE_API_ENGINE_EXPORT); // Tombstoned slots keep their type.

		auto* replacement = register_namespace("engine-probe");
		const auto replacement_slot = register_engine_export(replacement, "Probe", engine_probe, &provider);
		CHECK(replacement_slot > string_slot);
		CHECK(rml_native_api_publish_namespace(replacement) == RML_NATIVE_API_OK);
		CHECK(rml_native_api_revoke_namespace(replacement) == RML_NATIVE_API_OK);
	}

	void test_invalid_dispatch()
	{
		auto result = rml::native_api::dispatch_string(0, "x");
		CHECK(result.status == RML_NATIVE_API_INVALID_SLOT);
		CHECK(result.error_message == "unknown export slot");
		result = rml::native_api::dispatch_string(rml_native_api_max_export_slots + 1, "x");
		CHECK(result.status == RML_NATIVE_API_INVALID_SLOT);

		int result_count = -1;
		RmlNativeApiOwnedString error{};
		CHECK(rml_native_api_dispatch_engine(0, nullptr, &result_count, &error) == RML_NATIVE_API_INVALID_ARGUMENT);
		CHECK(result_count == 0);
		CHECK(error.data == nullptr && error.size == 0);
		rml_native_api_owned_string_destroy(&error);
	}

	void test_revoke_drains_in_flight_callback()
	{
		BlockContext context;
		auto* owner = register_namespace("blocking");
		const auto slot = register_export(owner, "Wait", blocking_callback, &context);
		CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_OK);
		RmlNativeApiStatus dispatch_status = RML_NATIVE_API_INVALID_ARGUMENT;
		std::thread dispatcher([&] {
			RmlNativeApiOwnedString value{};
			RmlNativeApiOwnedString error{};
			dispatch_status = rml_native_api_dispatch_string(slot, "", 0, &value, &error);
			rml_native_api_owned_string_destroy(&value);
			rml_native_api_owned_string_destroy(&error);
		});

		{
			std::unique_lock lock(context.mutex);
			CHECK(context.changed.wait_for(lock, std::chrono::seconds(2), [&] { return context.entered; }));
		}

		std::atomic_bool revoke_started{};
		RmlNativeApiStatus revoke_status = RML_NATIVE_API_INVALID_ARGUMENT;
		std::thread revoker([&] {
			revoke_started.store(true, std::memory_order_release);
			revoke_status = rml_native_api_revoke_namespace(owner);
		});
		while (!revoke_started.load(std::memory_order_acquire))
			std::this_thread::yield();

		bool saw_removed = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!saw_removed && std::chrono::steady_clock::now() < deadline)
		{
			auto result = rml::native_api::dispatch_string(slot, "later");
			saw_removed = result.status == RML_NATIVE_API_REMOVED_EXPORT;
			if (!saw_removed)
				std::this_thread::yield();
		}

		{
			std::scoped_lock lock(context.mutex);
			context.release = true;
		}
		context.changed.notify_all();
		dispatcher.join();
		revoker.join();
		CHECK(saw_removed);
		CHECK(dispatch_status == RML_NATIVE_API_OK);
		CHECK(revoke_status == RML_NATIVE_API_OK);
	}

	void test_slot_limit()
	{
		std::vector<RmlNativeApiOwner*> owners;
		RmlNativeApiExportSlot last_slot{};
		bool reached_limit = false;
		for (unsigned int index = 0; index < rml_native_api_max_export_slots + 2; ++index)
		{
			auto* owner = register_namespace("slot-" + std::to_string(index));
			RmlNativeApiExportSlot slot{};
			const auto status = rml_native_api_register_string_export(owner, "Call", echo, nullptr, &slot);
			if (status == RML_NATIVE_API_SLOT_LIMIT_REACHED)
			{
				CHECK(slot == 0);
				CHECK(rml_native_api_revoke_namespace(owner) == RML_NATIVE_API_OK);
				reached_limit = true;
				break;
			}
			CHECK(status == RML_NATIVE_API_OK);
			CHECK(slot > last_slot);
			CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_OK);
			last_slot = slot;
			owners.push_back(owner);
		}
		CHECK(reached_limit);
		CHECK(last_slot == rml_native_api_max_export_slots);
		for (auto* owner : owners)
			CHECK(rml_native_api_revoke_namespace(owner) == RML_NATIVE_API_OK);
		RmlNativeApiExportSlot slot{};
		CHECK(rml_native_api_register_string_export(
			owners.front(), "AfterRevoke", echo, nullptr, &slot) == RML_NATIVE_API_NAMESPACE_REVOKED);
		CHECK(slot == 0);
		auto* after_limit_owner = register_namespace("after-limit");
		CHECK(rml_native_api_register_string_export(
			after_limit_owner, "Call", echo, nullptr, &slot) == RML_NATIVE_API_SLOT_LIMIT_REACHED);
		CHECK(slot == 0);
		CHECK(rml_native_api_revoke_namespace(after_limit_owner) == RML_NATIVE_API_OK);
	}
}

int main()
{
	test_names_and_collisions();
	test_callback_failures();
	test_engine_exports();
	test_invalid_dispatch();
	test_revoke_drains_in_flight_callback();
	test_slot_limit();
	std::cout << "PASS: native API registry invariants\n";
	return 0;
}
