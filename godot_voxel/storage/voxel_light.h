#ifndef VOXEL_LIGHT_H
#define VOXEL_LIGHT_H

#include <cstdint>

namespace zylann::voxel {

// Helper struct for packing and unpacking 8-bit voxel light.
// 4 bits: Sunlight (0..15)
// 4 bits: Blocklight / Torchlight (0..15)
struct VoxelLight {
	static constexpr uint8_t MAX_LIGHT = 15;
	static constexpr uint8_t SUN_SHIFT = 4;
	static constexpr uint8_t SUN_MASK = 0xF0;
	static constexpr uint8_t BLOCK_MASK = 0x0F;

	// Pack 4-bit sunlight and 4-bit blocklight into a single 8-bit byte.
	static inline uint8_t pack(uint8_t sun, uint8_t block) {
		return ((sun & 0x0F) << SUN_SHIFT) | (block & 0x0F);
	}

	// Extract 4-bit sunlight (0..15).
	static inline uint8_t get_sun(uint8_t light) {
		return (light >> SUN_SHIFT) & 0x0F;
	}

	// Extract 4-bit blocklight (0..15).
	static inline uint8_t get_block(uint8_t light) {
		return light & BLOCK_MASK;
	}

	// Set sunlight (0..15) while preserving blocklight.
	static inline uint8_t set_sun(uint8_t light, uint8_t sun) {
		return (light & BLOCK_MASK) | ((sun & 0x0F) << SUN_SHIFT);
	}

	// Set blocklight (0..15) while preserving sunlight.
	static inline uint8_t set_block(uint8_t light, uint8_t block) {
		return (light & SUN_MASK) | (block & 0x0F);
	}

	// Normalize light level (0..15) to float (0.0..1.0).
	static inline float to_float(uint8_t level) {
		return static_cast<float>(level) * (1.0f / 15.0f);
	}
};

} // namespace zylann::voxel

#endif // VOXEL_LIGHT_H
