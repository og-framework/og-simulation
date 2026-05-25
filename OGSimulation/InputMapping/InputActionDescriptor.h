#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <vector>

namespace dInput
{

enum class ActionValueType : uint8_t
{
	Boolean,
	Axis1D,
	Axis2D,
	Axis3D
};

struct ActionDescriptor
{
	const char* name;
	ActionValueType valueType;
};

enum class KeyId : uint16_t
{
	// Keyboard
	Key_A, Key_B, Key_C, Key_D, Key_E, Key_F, Key_G, Key_H, Key_I, Key_J,
	Key_K, Key_L, Key_M, Key_N, Key_O, Key_P, Key_Q, Key_R, Key_S, Key_T,
	Key_U, Key_V, Key_W, Key_X, Key_Y, Key_Z,

	Key_Space,
	Key_LeftShift,

	// Mouse
	Mouse_X, Mouse_Y, Mouse_XY,
	Mouse_Left, Mouse_Right, Mouse_Middle,

	// Gamepad
	Gamepad_LeftStick_XY,
	Gamepad_RightStick_XY,
	Gamepad_FaceBottom,       // A / Cross
	Gamepad_FaceRight,        // B / Circle
	Gamepad_FaceLeft,         // X / Square
	Gamepad_FaceTop,          // Y / Triangle
	Gamepad_LeftTrigger,      // LT (digital)
	Gamepad_RightTrigger,     // RT (digital)
	Gamepad_LeftTriggerAxis,  // LT (analog)
	Gamepad_RightTriggerAxis, // RT (analog)
};

enum class KeyModifier : uint8_t
{
	None,
	Negate,
	SwizzleAxis,
	NegateAndSwizzle
};

struct KeyBinding
{
	KeyId key;
	KeyModifier modifier = KeyModifier::None;
};

struct ActionMapping
{
	const ActionDescriptor* action;
	std::vector<KeyBinding> bindings;
};

struct MappingContext
{
	const char* name;
	std::vector<ActionMapping> actionMappings;
};

} // namespace dInput
