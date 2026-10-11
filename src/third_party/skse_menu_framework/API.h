// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

// Minimal dynamic subset of QTR-Modding's SKSE Menu Framework 3 API header.
// The framework exports its menu registration and cimgui entry points from
// SKSEMenuFramework.dll. MPF resolves only the functions it uses, so the menu
// remains an optional runtime dependency.
// Upstream: https://github.com/QTR-Modding/SKSE-Menu-Framework-3-API

#include <Windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace SMF3
{
	struct Vec2
	{
		float x{ 0.0f };
		float y{ 0.0f };
	};

	struct Vec4
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
		float w{ 0.0f };
	};

	using RenderFunction = void(__stdcall*)();

	inline HMODULE Module()
	{
		return GetModuleHandleW(L"SKSEMenuFramework");
	}

	template <class T>
	inline T Function(const char* a_name)
	{
		const auto module = Module();
		return module ? reinterpret_cast<T>(GetProcAddress(module, a_name)) : nullptr;
	}

	inline bool IsAvailable()
	{
		return Module() && Function<void (*)(const char*, RenderFunction)>("AddSectionItem");
	}

	inline float ApiVersion()
	{
		const auto fn = Function<float (*)()>("GetMenuFrameworkVersion");
		return fn ? fn() : 0.0f;
	}

	inline bool AddPage(const char* a_path, RenderFunction a_render)
	{
		const auto fn = Function<void (*)(const char*, RenderFunction)>("AddSectionItem");
		if (!fn) {
			return false;
		}
		fn(a_path, a_render);
		return true;
	}

	namespace UI
	{
		// ImGuiCol values are part of the stable Dear ImGui enum ordering.  The
		// menu framework exposes cimgui directly, so keeping these tiny wrappers
		// here lets MPF provide clear action colours without linking ImGui.
		namespace Color
		{
			inline constexpr int Button = 21;
			inline constexpr int ButtonHovered = 22;
			inline constexpr int ButtonActive = 23;
		}

		inline bool PushStyleColor(int a_index, Vec4 a_color)
		{
			const auto fn = Function<void (*)(int, Vec4)>("igPushStyleColor_Vec4");
			if (!fn) {
				return false;
			}
			fn(a_index, a_color);
			return true;
		}

		inline void PopStyleColor(int a_count = 1)
		{
			if (const auto fn = Function<void (*)(int)>("igPopStyleColor")) {
				fn(a_count);
			}
		}

		inline void TextUnformatted(const char* a_text)
		{
			const auto fn = Function<void (*)(const char*, const char*)>("igTextUnformatted");
			if (fn) {
				fn(a_text, nullptr);
			}
		}

		inline void Text(const char* a_format, ...)
		{
			const auto fn = Function<void (*)(const char*, std::va_list)>("igTextV");
			if (!fn) {
				return;
			}
			std::va_list args;
			va_start(args, a_format);
			fn(a_format, args);
			va_end(args);
		}

		inline void TextDisabled(const char* a_format, ...)
		{
			const auto fn = Function<void (*)(const char*, std::va_list)>("igTextDisabledV");
			if (!fn) {
				return;
			}
			std::va_list args;
			va_start(args, a_format);
			fn(a_format, args);
			va_end(args);
		}

		inline void TextColored(Vec4 a_color, const char* a_format, ...)
		{
			const auto fn = Function<void (*)(Vec4, const char*, std::va_list)>("igTextColoredV");
			if (!fn) {
				return;
			}
			std::va_list args;
			va_start(args, a_format);
			fn(a_color, a_format, args);
			va_end(args);
		}

		inline bool Button(const char* a_label, Vec2 a_size = {})
		{
			const auto fn = Function<bool (*)(const char*, Vec2)>("igButton");
			return fn && fn(a_label, a_size);
		}

		inline bool SmallButton(const char* a_label)
		{
			const auto fn = Function<bool (*)(const char*)>("igSmallButton");
			return fn && fn(a_label);
		}

		inline bool ColoredButton(const char* a_label, Vec4 a_normal, Vec4 a_hover,
			Vec4 a_active, Vec2 a_size = {})
		{
			const bool pushed = PushStyleColor(Color::Button, a_normal) &&
				PushStyleColor(Color::ButtonHovered, a_hover) &&
				PushStyleColor(Color::ButtonActive, a_active);
			const bool clicked = Button(a_label, a_size);
			if (pushed) {
				PopStyleColor(3);
			}
			return clicked;
		}

		inline bool ColoredSmallButton(const char* a_label, Vec4 a_normal, Vec4 a_hover,
			Vec4 a_active)
		{
			const bool pushed = PushStyleColor(Color::Button, a_normal) &&
				PushStyleColor(Color::ButtonHovered, a_hover) &&
				PushStyleColor(Color::ButtonActive, a_active);
			const bool clicked = SmallButton(a_label);
			if (pushed) {
				PopStyleColor(3);
			}
			return clicked;
		}

		inline bool Checkbox(const char* a_label, bool* a_value)
		{
			const auto fn = Function<bool (*)(const char*, bool*)>("igCheckbox");
			return fn && fn(a_label, a_value);
		}

		inline bool InputText(const char* a_label, char* a_buffer, std::size_t a_size)
		{
			const auto fn = Function<bool (*)(const char*, char*, std::size_t, int, void*, void*)>("igInputText");
			return fn && fn(a_label, a_buffer, a_size, 0, nullptr, nullptr);
		}

		inline bool InputTextWithHint(const char* a_label, const char* a_hint, char* a_buffer,
			std::size_t a_size)
		{
			const auto fn = Function<bool (*)(const char*, const char*, char*, std::size_t, int, void*, void*)>("igInputTextWithHint");
			return fn && fn(a_label, a_hint, a_buffer, a_size, 0, nullptr, nullptr);
		}

		inline bool InputFloat(const char* a_label, float* a_value, const char* a_format = "%.1f")
		{
			const auto fn = Function<bool (*)(const char*, float*, float, float, const char*, int)>("igInputFloat");
			return fn && fn(a_label, a_value, 1.0f, 10.0f, a_format, 0);
		}

		inline bool InputInt(const char* a_label, int* a_value)
		{
			const auto fn = Function<bool (*)(const char*, int*, int, int, int)>("igInputInt");
			return fn && fn(a_label, a_value, 1, 10, 0);
		}

		inline bool BeginCombo(const char* a_label, const char* a_preview)
		{
			const auto fn = Function<bool (*)(const char*, const char*, int)>("igBeginCombo");
			return fn && fn(a_label, a_preview, 0);
		}

		inline void EndCombo()
		{
			if (const auto fn = Function<void (*)()>("igEndCombo")) {
				fn();
			}
		}

		inline bool Selectable(const char* a_label, bool a_selected = false)
		{
			const auto fn = Function<bool (*)(const char*, bool, int, Vec2)>("igSelectable_Bool");
			return fn && fn(a_label, a_selected, 0, {});
		}

		inline bool BeginTable(const char* a_id, int a_columns)
		{
			const auto fn = Function<bool (*)(const char*, int, int, Vec2, float)>("igBeginTable");
			return fn && fn(a_id, a_columns, 0, {}, 0.0f);
		}

		inline void EndTable()
		{
			if (const auto fn = Function<void (*)()>("igEndTable")) {
				fn();
			}
		}

		inline void TableSetupColumn(const char* a_label)
		{
			if (const auto fn = Function<void (*)(const char*, int, float, unsigned int)>("igTableSetupColumn")) {
				fn(a_label, 0, 0.0f, 0);
			}
		}

		inline void TableHeadersRow()
		{
			if (const auto fn = Function<void (*)()>("igTableHeadersRow")) {
				fn();
			}
		}

		inline void TableNextRow()
		{
			if (const auto fn = Function<void (*)(int, float)>("igTableNextRow")) {
				fn(0, 0.0f);
			}
		}

		inline bool TableNextColumn()
		{
			const auto fn = Function<bool (*)()>("igTableNextColumn");
			return fn && fn();
		}

		inline bool BeginTabBar(const char* a_id)
		{
			const auto fn = Function<bool (*)(const char*, int)>("igBeginTabBar");
			return fn && fn(a_id, 0);
		}

		inline void EndTabBar()
		{
			if (const auto fn = Function<void (*)()>("igEndTabBar")) {
				fn();
			}
		}

		inline bool BeginTabItem(const char* a_label)
		{
			const auto fn = Function<bool (*)(const char*, bool*, int)>("igBeginTabItem");
			return fn && fn(a_label, nullptr, 0);
		}

		inline void EndTabItem()
		{
			if (const auto fn = Function<void (*)()>("igEndTabItem")) {
				fn();
			}
		}

		inline void Separator()
		{
			if (const auto fn = Function<void (*)()>("igSeparator")) {
				fn();
			}
		}

		inline void SeparatorText(const char* a_label)
		{
			if (const auto fn = Function<void (*)(const char*)>("igSeparatorText")) {
				fn(a_label);
			}
		}

		inline void SameLine()
		{
			if (const auto fn = Function<void (*)(float, float)>("igSameLine")) {
				fn(0.0f, -1.0f);
			}
		}

		inline void SetScrollHereY(float a_centerY = 0.5f)
		{
			if (const auto fn = Function<void (*)(float)>("igSetScrollHereY")) {
				fn(a_centerY);
			}
		}

		inline void PushID(int a_id)
		{
			if (const auto fn = Function<void (*)(int)>("igPushID_Int")) {
				fn(a_id);
			}
		}

		inline void PopID()
		{
			if (const auto fn = Function<void (*)()>("igPopID")) {
				fn();
			}
		}
	}
}
