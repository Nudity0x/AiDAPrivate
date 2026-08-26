#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace test_lab {

	struct state_t;

	struct input_form_t {
		virtual ~input_form_t() = default;
		virtual void u32(const char* label, std::uint32_t* field, bool hex) = 0;
		virtual void u64(const char* label, std::uint64_t* field, bool hex) = 0;
		virtual void i32(const char* label, std::int32_t* field, std::int32_t lo, std::int32_t hi) = 0;
		virtual void text(const char* label, std::string* field, std::size_t max_len) = 0;
		virtual void combo(const char* label, std::uint32_t* field, const char* const* items, std::size_t count) = 0;
		virtual void checkbox_u32(const char* label, std::uint32_t* field, std::uint32_t on, std::uint32_t off) = 0;
		virtual void note(const char* text) = 0;
		virtual void action(const char* label, void(*fill)(state_t&)) = 0;
	};

}
