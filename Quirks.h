#pragma once
namespace Quirks
{
	static bool VFReset { true };
	static bool MemoryIncrement { false };
	static bool Clipping { true };
	static bool Shifting { true };
	static bool Jumping { false };

	inline void Reset()
	{
		VFReset = true;
		MemoryIncrement = false;
		Clipping = true;
		Shifting = true;
		Jumping = false;
	}
}