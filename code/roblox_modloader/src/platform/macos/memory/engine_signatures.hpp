#pragma once

#include "RobloxModLoader/memory/all.hpp"
#include "pointers.hpp"

#include <stdexcept>

namespace rml
{
	constexpr auto Pointers::get_roblox_batch()
	{
		// clang-format off
		constexpr auto batch_and_hash = memory::make_batch<
			{
				"MENU_BUILD_FROM_DOM",
				"FF C3 02 D1 FC 6F 05 A9 FA 67 06 A9 F8 5F 07 A9 F6 57 08 A9 F4 4F 09 A9 FD 7B 0A A9 FD 83 02 91 F4 03 01 AA F3 03 00 AA E2 17 00 F9",
				[](const memory::handle ptr) {
					g_pointers->m_roblox_pointers.build_menu_bar_from_dom = ptr.as<functions::build_menu_bar_from_dom>();
				},
			},
			{
				"GET_STRING_ATOM",
				"F4 4F BE A9 FD 7B 01 A9 FD 43 00 91 F3 03 00 AA ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? E1 03 13 AA FD 7B 41 A9 F4 4F C2 A8 ? ? ? ? FF C3 00 D1 F4 4F 01 A9 FD 7B 02 A9 FD 83 00 91 F3 03 00 AA ? ? ? ? F4 03 01 AA",
				[](const memory::handle ptr) {
					g_pointers->m_roblox_pointers.get_string_atom = ptr.as<functions::get_string_atom>();
				},
			},
			{
				"DESCRIPTOR_LOOKUP",
				"FF 43 01 D1 F6 57 02 A9 F4 4F 03 A9 FD 7B 04 A9 FD 03 01 91 F4 03 01 AA F3 03 00 AA 21 10 40 F9",
				[](const memory::handle ptr) {
					// Studio 0.741 ARM64: resolve the lookup target and member-table
					// offset from its caller. Based on upstream 0d4b8968c89e;
					// all four signatures are unique in 0.741.19.7411056.
					const auto member_address = ptr.add(0x70).as<const std::uint32_t&>();
					const auto lookup_call = ptr.add(0x78).as<const std::uint32_t&>();
					// Require ADD x0, Xn, #imm12 (unshifted) followed by BL before
					// interpreting either operand. A changed caller must fail closed.
					if ((member_address & 0xFFC0001F) != 0x91000000
					    || (lookup_call & 0xFC000000) != 0x94000000)
					{
						throw std::runtime_error("DESCRIPTOR_LOOKUP caller instructions changed");
					}
					g_pointers->m_roblox_pointers.descriptor_lookup = ptr.add(0x78).bl().as<functions::descriptor_lookup>();
					g_pointers->m_roblox_pointers.member_table_offset = (member_address >> 10) & 0xFFF;
				},
			},
			{
				"OBJECT_CREATE_BY_NAME",
				"FF 03 01 D1 F6 57 01 A9 F4 4F 02 A9 FD 7B 03 A9 FD C3 00 91 F4 03 02 AA F5 03 00 AA F3 03 08 AA E0 03 01 AA ? ? ? ? ? ? ? ? F6 03 00 AA 9F 06 00 71 ? ? ? ? ? ? ? ? 9F 06 00 71 ? ? ? ? C8 02 40 F9 08 05 40 F9 E0 03 16 AA 00 01 3F D6 ? ? ? ? ? ? ? ? 9F 0E 00 71 ? ? ? ? 9F 0A 00 71 ? ? ? ? C8 02 40 F9 08 09 40 F9 E0 03 16 AA 00 01 3F D6 ? ? ? ? C8 02 40 F9 09 01 40 F9 E8 03 00 91 E0 03 16 AA E1 03 15 AA E2 03 14 AA 20 01 3F D6 E0 03 00 91",
				[](const memory::handle ptr) {
					g_pointers->m_roblox_pointers.object_create_by_name = ptr.as<functions::object_create_by_name>();
				},
			}
		>();
		// clang-format on

		return batch_and_hash;
	}
}
