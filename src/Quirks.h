#pragma once

namespace Quirks
{
	static bool VFReset { true };
	static bool MemoryIncrement { true };
	static bool Clipping { true };
	static bool Shifting { false };
	static bool Jumping { false };

	inline void Reset()
	{
		VFReset = true;
		MemoryIncrement = true;
		Clipping = true;
		Shifting = false;
		Jumping = false;
	}
}