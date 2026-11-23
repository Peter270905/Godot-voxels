#include "voxel_light_engine.h"
#include "../util/math/funcs.h"

namespace zylann::voxel {

// Down neighbor is checked first so vertical daylight column reaches ground before lateral diffusion
static const Vector3i NEIGHBORS[6] = {
	Vector3i(0, -1, 0),
	Vector3i(1, 0, 0),
	Vector3i(-1, 0, 0),
	Vector3i(0, 0, 1),
	Vector3i(0, 0, -1),
	Vector3i(0, 1, 0)
};

void VoxelLightEngine::clear_queues() {
	_block_propagate_queue.clear();
	_block_remove_queue.clear();
	_sun_propagate_queue.clear();
	_sun_remove_queue.clear();
}

// -------------------------------------------------------------------------------------------------
// Templated BFS propagation loops (inlined for VoxelBufferLightAccessor or generic accessors)
// -------------------------------------------------------------------------------------------------

template <typename TAccessor>
inline void propagate_block_light_template(
		TAccessor &accessor, FastQueue<VoxelLightEngine::LightNode> &queue, const VoxelLightEngine *engine) {
	const blocky::BakedLibrary *lib = accessor.get_library();

	while (!queue.empty()) {
		const VoxelLightEngine::LightNode node = queue.front();
		queue.pop();

		if (node.level <= 1) {
			continue;
		}

		for (int i = 0; i < 6; ++i) {
			const Vector3i npos = node.pos + NEIGHBORS[i];
			if (!accessor.is_valid_pos(npos)) {
				continue;
			}

			const uint16_t ntype = accessor.get_type_unchecked(npos);
			const uint8_t filter = engine->get_filter(lib, ntype);
			if (filter >= 15) {
				continue;
			}

			const uint8_t attenuation = (filter < 1) ? 1 : filter;
			if (node.level <= attenuation) {
				continue;
			}
			const uint8_t new_light = node.level - attenuation;

			const uint8_t cur_packed = accessor.get_light_unchecked(npos);
			const uint8_t cur_block_light = VoxelLight::get_block(cur_packed);

			if (new_light > cur_block_light) {
				accessor.set_light_unchecked(npos, VoxelLight::set_block(cur_packed, new_light));
				queue.push({ npos, new_light });
			}
		}
	}
}

template <typename TAccessor>
inline void propagate_sun_light_template(
		TAccessor &accessor, FastQueue<VoxelLightEngine::LightNode> &queue, const VoxelLightEngine *engine) {
	const blocky::BakedLibrary *lib = accessor.get_library();

	while (!queue.empty()) {
		const VoxelLightEngine::LightNode node = queue.front();
		queue.pop();

		if (node.level <= 1) {
			continue;
		}

		for (int i = 0; i < 6; ++i) {
			const Vector3i npos = node.pos + NEIGHBORS[i];
			if (!accessor.is_valid_pos(npos)) {
				continue;
			}

			const uint16_t ntype = accessor.get_type_unchecked(npos);
			const uint8_t filter = engine->get_filter(lib, ntype);
			if (filter >= 15) {
				continue;
			}

			// Minecraft rule: direct sunlight ray of level 15 going downwards through transparent block (filter == 0) does not decay!
			uint8_t attenuation;
			if (filter == 0 && node.level == 15 && npos.y < node.pos.y) {
				attenuation = 0;
			} else {
				attenuation = (filter < 1) ? 1 : filter;
			}

			if (node.level <= attenuation) {
				continue;
			}
			const uint8_t new_light = node.level - attenuation;

			const uint8_t cur_packed = accessor.get_light_unchecked(npos);
			const uint8_t cur_sun_light = VoxelLight::get_sun(cur_packed);

			if (new_light > cur_sun_light) {
				accessor.set_light_unchecked(npos, VoxelLight::set_sun(cur_packed, new_light));
				queue.push({ npos, new_light });
			}
		}
	}
}

// -------------------------------------------------------------------------------------------------
// Block Light (Torches, Fire, Shrooms)
// -------------------------------------------------------------------------------------------------

void VoxelLightEngine::add_block_light(IVoxelLightAccessor &accessor, Vector3i pos, uint8_t emission) {
	if (emission == 0 || !accessor.is_valid_pos(pos)) {
		return;
	}
	const uint8_t cur_light = accessor.get_light(pos);
	accessor.set_light(pos, VoxelLight::set_block(cur_light, emission));
	_block_propagate_queue.push({ pos, emission });
	propagate_block_light(accessor);
}

void VoxelLightEngine::propagate_block_light(IVoxelLightAccessor &accessor) {
	propagate_block_light_template(accessor, _block_propagate_queue, this);
}

void VoxelLightEngine::remove_block_light(IVoxelLightAccessor &accessor, Vector3i pos, uint8_t old_level) {
	if (old_level == 0 || !accessor.is_valid_pos(pos)) {
		return;
	}

	_block_remove_queue.push({ pos, old_level });
	const uint8_t cur = accessor.get_light(pos);
	accessor.set_light(pos, VoxelLight::set_block(cur, 0));

	while (!_block_remove_queue.empty()) {
		const LightNode node = _block_remove_queue.front();
		_block_remove_queue.pop();

		for (int i = 0; i < 6; ++i) {
			const Vector3i npos = node.pos + NEIGHBORS[i];
			if (!accessor.is_valid_pos(npos)) {
				continue;
			}

			const uint8_t cur_packed = accessor.get_light(npos);
			const uint8_t neighbor_level = VoxelLight::get_block(cur_packed);

			if (neighbor_level != 0 && neighbor_level < node.level) {
				// Cascading light reduction
				accessor.set_light(npos, VoxelLight::set_block(cur_packed, 0));
				_block_remove_queue.push({ npos, neighbor_level });
			} else if (neighbor_level >= node.level) {
				// Re-propagate from neighboring light sources
				_block_propagate_queue.push({ npos, neighbor_level });
			}
		}
	}

	propagate_block_light(accessor);
}

// -------------------------------------------------------------------------------------------------
// Sunlight (Sky & Day/Night Spread)
// -------------------------------------------------------------------------------------------------

void VoxelLightEngine::add_sun_light(IVoxelLightAccessor &accessor, Vector3i pos, uint8_t level) {
	if (level == 0 || !accessor.is_valid_pos(pos)) {
		return;
	}
	const uint8_t cur_light = accessor.get_light(pos);
	accessor.set_light(pos, VoxelLight::set_sun(cur_light, level));
	_sun_propagate_queue.push({ pos, level });
	propagate_sun_light(accessor);
}

void VoxelLightEngine::propagate_sun_light(IVoxelLightAccessor &accessor) {
	propagate_sun_light_template(accessor, _sun_propagate_queue, this);
}

void VoxelLightEngine::remove_sun_light(IVoxelLightAccessor &accessor, Vector3i pos, uint8_t old_level) {
	if (old_level == 0 || !accessor.is_valid_pos(pos)) {
		return;
	}

	_sun_remove_queue.push({ pos, old_level });
	const uint8_t cur = accessor.get_light(pos);
	accessor.set_light(pos, VoxelLight::set_sun(cur, 0));

	if (old_level == 15) {
		const blocky::BakedLibrary *lib = accessor.get_library();
		Vector3i down_pos = pos + Vector3i(0, -1, 0);
		while (accessor.is_valid_pos(down_pos)) {
			const uint8_t d_light = accessor.get_light(down_pos);
			const uint8_t d_sun = VoxelLight::get_sun(d_light);
			if (d_sun != 15) {
				break;
			}
			accessor.set_light(down_pos, VoxelLight::set_sun(d_light, 0));
			_sun_remove_queue.push({ down_pos, 15 });
			if (get_filter(lib, accessor.get_type(down_pos)) >= 15) {
				break;
			}
			down_pos.y -= 1;
		}
	}

	while (!_sun_remove_queue.empty()) {
		const LightNode node = _sun_remove_queue.front();
		_sun_remove_queue.pop();

		for (int i = 0; i < 6; ++i) {
			const Vector3i npos = node.pos + NEIGHBORS[i];
			if (!accessor.is_valid_pos(npos)) {
				continue;
			}

			const uint8_t cur_packed = accessor.get_light(npos);
			const uint8_t neighbor_level = VoxelLight::get_sun(cur_packed);

			if (neighbor_level != 0 && neighbor_level < node.level) {
				accessor.set_light(npos, VoxelLight::set_sun(cur_packed, 0));
				_sun_remove_queue.push({ npos, neighbor_level });
			} else if (neighbor_level >= node.level) {
				_sun_propagate_queue.push({ npos, neighbor_level });
			}
		}
	}

	propagate_sun_light(accessor);
}

// -------------------------------------------------------------------------------------------------
// High-level Block Placed / Removed Notifications
// -------------------------------------------------------------------------------------------------

void VoxelLightEngine::on_block_placed(IVoxelLightAccessor &accessor, Vector3i pos, uint16_t new_type) {
	if (!accessor.is_valid_pos(pos)) {
		return;
	}
	const blocky::BakedLibrary *lib = accessor.get_library();
	const uint8_t filter = get_filter(lib, new_type);
	const uint8_t emission = get_emission(lib, new_type);

	const uint8_t old_packed = accessor.get_light(pos);
	const uint8_t old_block_light = VoxelLight::get_block(old_packed);
	const uint8_t old_sun_light = VoxelLight::get_sun(old_packed);

	if (filter >= 15) {
		if (old_block_light > 0) {
			remove_block_light(accessor, pos, old_block_light);
		}
		if (old_sun_light > 0) {
			remove_sun_light(accessor, pos, old_sun_light);
		}
	}

	if (emission > 0) {
		add_block_light(accessor, pos, emission);
	}
}

void VoxelLightEngine::on_block_removed(IVoxelLightAccessor &accessor, Vector3i pos, uint16_t old_type) {
	if (!accessor.is_valid_pos(pos)) {
		return;
	}
	const blocky::BakedLibrary *lib = accessor.get_library();
	const uint8_t old_emission = get_emission(lib, old_type);

	if (old_emission > 0) {
		remove_block_light(accessor, pos, old_emission);
	}

	// Sunlight vertical ray check: if block above has sun=15, beam continues straight down
	const Vector3i up_pos = pos + Vector3i(0, 1, 0);
	if (accessor.is_valid_pos(up_pos)) {
		const uint8_t up_sun = VoxelLight::get_sun(accessor.get_light(up_pos));
		if (up_sun == 15) {
			Vector3i down_pos = pos;
			while (accessor.is_valid_pos(down_pos)) {
				const uint16_t d_type = accessor.get_type(down_pos);
				if (get_filter(lib, d_type) >= 15) {
					break;
				}
				const uint8_t light = accessor.get_light(down_pos);
				accessor.set_light(down_pos, VoxelLight::set_sun(light, 15));
				_sun_propagate_queue.push({ down_pos, 15 });
				down_pos.y -= 1;
			}
		}
	}

	propagate_block_light(accessor);
	propagate_sun_light(accessor);
}

// -------------------------------------------------------------------------------------------------
// Full Buffer / Chunk Lighting Calculation
// -------------------------------------------------------------------------------------------------

void VoxelLightEngine::calculate_buffer_lighting(
		VoxelBuffer &buffer, const blocky::BakedLibrary *library, bool calculate_sunlight) {
	if (buffer.is_uniform(VoxelBuffer::CHANNEL_TYPE)) {
		const uint16_t type = static_cast<uint16_t>(buffer.get_voxel(0, 0, 0, VoxelBuffer::CHANNEL_TYPE));
		const uint8_t emission = get_emission(library, type);
		if (emission == 0) {
			if (type == 0 && calculate_sunlight) {
				buffer.fill(VoxelLight::set_sun(0, 15), VoxelBuffer::CHANNEL_LIGHT);
			} else {
				buffer.fill(0, VoxelBuffer::CHANNEL_LIGHT);
			}
			return;
		}
	}

	VoxelBufferLightAccessor accessor(buffer, library);
	const Vector3i size = buffer.get_size();

	clear_queues();

	// 1. Fast flat scan for block light emitters
	const size_t volume = static_cast<size_t>(size.x * size.y * size.z);
	if (accessor.has_types_16()) {
		Span<const uint16_t> types = accessor.get_types_16();
		for (size_t i = 0; i < volume; ++i) {
			const uint16_t type = types[i];
			const uint8_t emission = get_emission(library, type);
			if (emission > 0) {
				const int y = static_cast<int>(i % size.y);
				const size_t xz = i / size.y;
				const int x = static_cast<int>(xz % size.x);
				const int z = static_cast<int>(xz / size.x);
				const uint8_t cur = accessor.get_light_fast_index(i);
				accessor.set_light_fast_index(i, VoxelLight::set_block(cur, emission));
				_block_propagate_queue.push({ Vector3i(x, y, z), emission });
			}
		}
	} else {
		for (int z = 0; z < size.z; ++z) {
			for (int x = 0; x < size.x; ++x) {
				for (int y = 0; y < size.y; ++y) {
					const uint16_t type = accessor.get_type_fast(x, y, z);
					const uint8_t emission = get_emission(library, type);
					if (emission > 0) {
						const uint8_t cur = accessor.get_light_fast(x, y, z);
						accessor.set_light_fast(x, y, z, VoxelLight::set_block(cur, emission));
						_block_propagate_queue.push({ Vector3i(x, y, z), emission });
					}
				}
			}
		}
	}

	// 2. Scan sunlight columns (straight down from sky to first non-transparent block)
	if (calculate_sunlight) {
		StdVector<int16_t> bottom_y;
		bottom_y.resize(size.x * size.z, size.y);

		for (int z = 0; z < size.z; ++z) {
			for (int x = 0; x < size.x; ++x) {
				int last_y = size.y;
				for (int y = size.y - 1; y >= 0; --y) {
					const uint16_t type = accessor.get_type_fast(x, y, z);
					const uint8_t filter = get_filter(library, type);
					if (filter > 0) {
						// Blocked: anything not fully transparent stops primary beam (Phase 1.4)
						break;
					}
					const uint8_t cur = accessor.get_light_fast(x, y, z);
					accessor.set_light_fast(x, y, z, VoxelLight::set_sun(cur, 15));
					last_y = y;
				}
				bottom_y[x + z * size.x] = last_y;
			}
		}

		// Queue only boundary sky voxels that can propagate
		_sun_propagate_queue.reserve(1024);
		for (int z = 0; z < size.z; ++z) {
			for (int x = 0; x < size.x; ++x) {
				const int b_y = bottom_y[x + z * size.x];
				if (b_y >= size.y) {
					continue;
				}

				int max_neighbor_bottom = b_y;
				if (x > 0) {
					max_neighbor_bottom = math::max(max_neighbor_bottom, static_cast<int>(bottom_y[(x - 1) + z * size.x]));
				}
				if (x + 1 < size.x) {
					max_neighbor_bottom = math::max(max_neighbor_bottom, static_cast<int>(bottom_y[(x + 1) + z * size.x]));
				}
				if (z > 0) {
					max_neighbor_bottom = math::max(max_neighbor_bottom, static_cast<int>(bottom_y[x + (z - 1) * size.x]));
				}
				if (z + 1 < size.z) {
					max_neighbor_bottom = math::max(max_neighbor_bottom, static_cast<int>(bottom_y[x + (z + 1) * size.x]));
				}

				const int queue_end = math::min(static_cast<int>(size.y), max_neighbor_bottom + 1);
				for (int y = b_y; y < queue_end; ++y) {
					_sun_propagate_queue.push({ Vector3i(x, y, z), 15 });
				}
			}
		}
	}

	// 3. Propagate both queues with inlined fast accessor
	propagate_sun_light_template(accessor, _sun_propagate_queue, this);
	propagate_block_light_template(accessor, _block_propagate_queue, this);
}

void VoxelLightEngine::calculate_buffer_lighting_with_heightmap(
		VoxelBuffer &buffer,
		const blocky::BakedLibrary *library,
		Span<const int32_t> heightmap,
		int origin_y
) {
	if (buffer.is_uniform(VoxelBuffer::CHANNEL_TYPE)) {
		const uint16_t type = static_cast<uint16_t>(buffer.get_voxel(0, 0, 0, VoxelBuffer::CHANNEL_TYPE));
		const uint8_t emission = get_emission(library, type);
		if (emission == 0 && type != 0) {
			buffer.fill(0, VoxelBuffer::CHANNEL_LIGHT);
			return;
		}
	}

	VoxelBufferLightAccessor accessor(buffer, library);
	const Vector3i size = buffer.get_size();

	clear_queues();

	// 1. Fast flat scan for block light emitters
	const size_t volume = static_cast<size_t>(size.x * size.y * size.z);
	if (accessor.has_types_16()) {
		Span<const uint16_t> types = accessor.get_types_16();
		for (size_t i = 0; i < volume; ++i) {
			const uint16_t type = types[i];
			const uint8_t emission = get_emission(library, type);
			if (emission > 0) {
				const int y = static_cast<int>(i % size.y);
				const size_t xz = i / size.y;
				const int x = static_cast<int>(xz % size.x);
				const int z = static_cast<int>(xz / size.x);
				const uint8_t cur = accessor.get_light_fast_index(i);
				accessor.set_light_fast_index(i, VoxelLight::set_block(cur, emission));
				_block_propagate_queue.push({ Vector3i(x, y, z), emission });
			}
		}
	} else {
		for (int z = 0; z < size.z; ++z) {
			for (int x = 0; x < size.x; ++x) {
				for (int y = 0; y < size.y; ++y) {
					const uint16_t type = accessor.get_type_fast(x, y, z);
					const uint8_t emission = get_emission(library, type);
					if (emission > 0) {
						const uint8_t cur = accessor.get_light_fast(x, y, z);
						accessor.set_light_fast(x, y, z, VoxelLight::set_block(cur, emission));
						_block_propagate_queue.push({ Vector3i(x, y, z), emission });
					}
				}
			}
		}
	}

	// 2. Scan sunlight columns based on heightmap
	const bool has_heightmap = heightmap.size() >= static_cast<size_t>(size.x * size.z);
	StdVector<int16_t> bottom_y;
	bottom_y.resize(size.x * size.z, size.y);

	for (int z = 0; z < size.z; ++z) {
		for (int x = 0; x < size.x; ++x) {
			int surface_local_y = size.y; // Default: all sky
			if (has_heightmap) {
				const int surface_world_y = heightmap[x + z * size.x];
				surface_local_y = surface_world_y - origin_y;
			}

			// If the surface is at or above the top of this buffer, this column is underground!
			if (surface_local_y >= size.y) {
				continue;
			}

			// From top down to surface: sunlight = 15
			int last_y = size.y;
			for (int y = size.y - 1; y >= 0; --y) {
				if (y <= surface_local_y) {
					// Reached surface ground level, stop vertical sunlight
					break;
				}
				const uint16_t type = accessor.get_type_fast(x, y, z);
				const uint8_t filter = get_filter(library, type);
				if (filter > 0) {
					// Hit a non-fully-transparent block (e.g. tree leaf, roof, water) above the ground (Phase 1.4)
					break;
				}
				const uint8_t cur = accessor.get_light_fast(x, y, z);
				accessor.set_light_fast(x, y, z, VoxelLight::set_sun(cur, 15));
				last_y = y;
			}
			bottom_y[x + z * size.x] = last_y;
		}
	}

	// Queue only boundary sky voxels that can propagate
	_sun_propagate_queue.reserve(1024);
	for (int z = 0; z < size.z; ++z) {
		for (int x = 0; x < size.x; ++x) {
			const int b_y = bottom_y[x + z * size.x];
			if (b_y >= size.y) {
				continue;
			}

			int max_neighbor_bottom = b_y;
			if (x > 0) {
				max_neighbor_bottom = math::max(max_neighbor_bottom, static_cast<int>(bottom_y[(x - 1) + z * size.x]));
			}
			if (x + 1 < size.x) {
				max_neighbor_bottom = math::max(max_neighbor_bottom, static_cast<int>(bottom_y[(x + 1) + z * size.x]));
			}
			if (z > 0) {
				max_neighbor_bottom = math::max(max_neighbor_bottom, static_cast<int>(bottom_y[x + (z - 1) * size.x]));
			}
			if (z + 1 < size.z) {
				max_neighbor_bottom = math::max(max_neighbor_bottom, static_cast<int>(bottom_y[x + (z + 1) * size.x]));
			}

			const int queue_end = math::min(static_cast<int>(size.y), max_neighbor_bottom + 1);
			for (int y = b_y; y < queue_end; ++y) {
				_sun_propagate_queue.push({ Vector3i(x, y, z), 15 });
			}
		}
	}

	// 3. Propagate both queues with inlined fast accessor
	propagate_sun_light_template(accessor, _sun_propagate_queue, this);
	propagate_block_light_template(accessor, _block_propagate_queue, this);
}

} // namespace zylann::voxel
