#ifndef VOXEL_LIGHT_ENGINE_H
#define VOXEL_LIGHT_ENGINE_H

#include "../meshers/blocky/blocky_baked_library.h"
#include "../util/containers/std_vector.h"
#include "../util/math/vector3i.h"
#include "voxel_buffer.h"
#include "voxel_light.h"

namespace zylann::voxel {

// Lightweight flat vector queue with head index to eliminate heap reallocations during BFS.
template <typename T>
class FastQueue {
public:
	inline void push(const T &item) {
		_data.push_back(item);
	}

	inline const T &front() const {
		return _data[_head];
	}

	inline void pop() {
		++_head;
	}

	inline bool empty() const {
		return _head >= _data.size();
	}

	inline size_t size() const {
		return _data.size() - _head;
	}

	inline void clear() {
		_data.clear();
		_head = 0;
	}

	inline void reserve(size_t capacity) {
		_data.reserve(capacity);
	}

private:
	StdVector<T> _data;
	size_t _head = 0;
};

// Interface for querying and updating voxel types and light values during propagation.
class IVoxelLightAccessor {
public:
	virtual ~IVoxelLightAccessor() = default;

	// Query block type ID at position. Returns 0 if empty or out of bounds.
	virtual uint16_t get_type(Vector3i pos) = 0;

	// Query packed 8-bit light (4-bit sun, 4-bit block).
	virtual uint8_t get_light(Vector3i pos) = 0;

	// Set packed 8-bit light.
	virtual void set_light(Vector3i pos, uint8_t light) = 0;

	// Check if position is accessible and within bounds.
	virtual bool is_valid_pos(Vector3i pos) const = 0;

	// Unchecked accessors for fast path when is_valid_pos was already verified
	virtual inline uint16_t get_type_unchecked(Vector3i pos) {
		return get_type(pos);
	}
	virtual inline uint8_t get_light_unchecked(Vector3i pos) {
		return get_light(pos);
	}
	virtual inline void set_light_unchecked(Vector3i pos, uint8_t light) {
		set_light(pos, light);
	}

	// Get baked library for block property lookups.
	virtual const blocky::BakedLibrary *get_library() const = 0;
};

// Accessor implementation for operating directly on a single VoxelBuffer using direct flat array slices.
class VoxelBufferLightAccessor : public IVoxelLightAccessor {
public:
	VoxelBufferLightAccessor(VoxelBuffer &buffer, const blocky::BakedLibrary *library, Vector3i offset = Vector3i()) :
			_buffer(buffer), _library(library), _offset(offset) {
		_size = buffer.get_size();
		_stride_y = 1;
		_stride_x = _size.y;
		_stride_z = _size.y * _size.x;

		buffer.decompress_channel(VoxelBuffer::CHANNEL_LIGHT);
		buffer.get_channel_as_bytes(VoxelBuffer::CHANNEL_LIGHT, _light_data);

		const VoxelBuffer::Depth type_depth = buffer.get_channel_depth(VoxelBuffer::CHANNEL_TYPE);
		if (buffer.get_channel_compression(VoxelBuffer::CHANNEL_TYPE) != VoxelBuffer::COMPRESSION_UNIFORM) {
			if (type_depth == VoxelBuffer::DEPTH_16_BIT) {
				buffer.get_channel_data_read_only(VoxelBuffer::CHANNEL_TYPE, _types_16);
			} else if (type_depth == VoxelBuffer::DEPTH_8_BIT) {
				buffer.get_channel_data_read_only(VoxelBuffer::CHANNEL_TYPE, _types_8);
			} else if (type_depth == VoxelBuffer::DEPTH_32_BIT) {
				buffer.get_channel_data_read_only(VoxelBuffer::CHANNEL_TYPE, _types_32);
			}
		} else {
			_uniform_type = static_cast<uint16_t>(buffer.get_voxel(0, 0, 0, VoxelBuffer::CHANNEL_TYPE));
		}
	}

	inline size_t get_flat_index(int lx, int ly, int lz) const {
		return ly + lx * _stride_x + lz * _stride_z;
	}

	inline bool has_types_16() const {
		return _types_16.data() != nullptr;
	}

	inline Span<const uint16_t> get_types_16() const {
		return _types_16;
	}

	inline uint16_t get_type_fast(int lx, int ly, int lz) const {
		const size_t idx = get_flat_index(lx, ly, lz);
		if (_types_16.data() != nullptr) {
			return _types_16[idx];
		} else if (_types_8.data() != nullptr) {
			return _types_8[idx];
		} else if (_types_32.data() != nullptr) {
			return _types_32[idx];
		}
		return _uniform_type;
	}

	inline uint8_t get_light_fast(int lx, int ly, int lz) const {
		return _light_data[get_flat_index(lx, ly, lz)];
	}

	inline void set_light_fast(int lx, int ly, int lz, uint8_t light) {
		_light_data[get_flat_index(lx, ly, lz)] = light;
	}

	inline uint8_t get_light_fast_index(size_t idx) const {
		return _light_data[idx];
	}

	inline void set_light_fast_index(size_t idx, uint8_t light) {
		_light_data[idx] = light;
	}

	inline uint16_t get_type_unchecked(Vector3i pos) override {
		return get_type_fast(pos.x - _offset.x, pos.y - _offset.y, pos.z - _offset.z);
	}

	inline uint8_t get_light_unchecked(Vector3i pos) override {
		return get_light_fast(pos.x - _offset.x, pos.y - _offset.y, pos.z - _offset.z);
	}

	inline void set_light_unchecked(Vector3i pos, uint8_t light) override {
		set_light_fast(pos.x - _offset.x, pos.y - _offset.y, pos.z - _offset.z, light);
	}

	inline uint16_t get_type(Vector3i pos) override {
		const Vector3i local = pos - _offset;
		if (static_cast<unsigned int>(local.x) >= static_cast<unsigned int>(_size.x) ||
			static_cast<unsigned int>(local.y) >= static_cast<unsigned int>(_size.y) ||
			static_cast<unsigned int>(local.z) >= static_cast<unsigned int>(_size.z)) {
			return 0;
		}
		return get_type_fast(local.x, local.y, local.z);
	}

	inline uint8_t get_light(Vector3i pos) override {
		const Vector3i local = pos - _offset;
		if (static_cast<unsigned int>(local.x) >= static_cast<unsigned int>(_size.x) ||
			static_cast<unsigned int>(local.y) >= static_cast<unsigned int>(_size.y) ||
			static_cast<unsigned int>(local.z) >= static_cast<unsigned int>(_size.z)) {
			return 0;
		}
		return get_light_fast(local.x, local.y, local.z);
	}

	inline void set_light(Vector3i pos, uint8_t light) override {
		const Vector3i local = pos - _offset;
		if (static_cast<unsigned int>(local.x) >= static_cast<unsigned int>(_size.x) ||
			static_cast<unsigned int>(local.y) >= static_cast<unsigned int>(_size.y) ||
			static_cast<unsigned int>(local.z) >= static_cast<unsigned int>(_size.z)) {
			return;
		}
		set_light_fast(local.x, local.y, local.z, light);
	}

	inline bool is_valid_pos(Vector3i pos) const override {
		const Vector3i local = pos - _offset;
		return static_cast<unsigned int>(local.x) < static_cast<unsigned int>(_size.x) &&
			   static_cast<unsigned int>(local.y) < static_cast<unsigned int>(_size.y) &&
			   static_cast<unsigned int>(local.z) < static_cast<unsigned int>(_size.z);
	}

	const blocky::BakedLibrary *get_library() const override {
		return _library;
	}

	inline Vector3i get_size() const {
		return _size;
	}
	inline Vector3i get_offset() const {
		return _offset;
	}
	inline Span<uint8_t> get_light_data() {
		return _light_data;
	}

private:
	VoxelBuffer &_buffer;
	const blocky::BakedLibrary *_library;
	Vector3i _offset;
	Vector3i _size;
	int _stride_x = 0;
	int _stride_y = 1;
	int _stride_z = 0;
	Span<uint8_t> _light_data;
	Span<const uint16_t> _types_16;
	Span<const uint8_t> _types_8;
	Span<const uint32_t> _types_32;
	uint16_t _uniform_type = 0;
};

// Breadth-First Search (BFS) Voxel Lighting Engine.
class VoxelLightEngine {
public:
	struct LightNode {
		Vector3i pos;
		uint8_t level;
	};

	// 1. Blocklight propagation and removal
	void add_block_light(IVoxelLightAccessor &accessor, Vector3i pos, uint8_t emission);
	void remove_block_light(IVoxelLightAccessor &accessor, Vector3i pos, uint8_t old_level);
	void propagate_block_light(IVoxelLightAccessor &accessor);

	// 2. Sunlight propagation and removal
	void add_sun_light(IVoxelLightAccessor &accessor, Vector3i pos, uint8_t level);
	void remove_sun_light(IVoxelLightAccessor &accessor, Vector3i pos, uint8_t old_level);
	void propagate_sun_light(IVoxelLightAccessor &accessor);

	// 3. Block placement and destruction helpers
	void on_block_placed(IVoxelLightAccessor &accessor, Vector3i pos, uint16_t new_type);
	void on_block_removed(IVoxelLightAccessor &accessor, Vector3i pos, uint16_t old_type);

	// 4. Batch lighting calculation for a single buffer/chunk
	void calculate_buffer_lighting(VoxelBuffer &buffer, const blocky::BakedLibrary *library, bool calculate_sunlight = true);
	void calculate_buffer_lighting_with_heightmap(
			VoxelBuffer &buffer,
			const blocky::BakedLibrary *library,
			Span<const int32_t> heightmap,
			int origin_y
	);

	// Property helpers (inlined for zero call overhead)
	inline uint8_t get_filter(const blocky::BakedLibrary *lib, uint16_t type) const {
		if (type == 0) {
			return 0; // Air default (fully transparent)
		}
		if (lib != nullptr && lib->has_model(type)) {
			return lib->models[type].light_filter;
		}
		return 15; // Solid opaque default
	}

	inline uint8_t get_emission(const blocky::BakedLibrary *lib, uint16_t type) const {
		if (lib != nullptr && lib->has_model(type)) {
			return lib->models[type].light_emission;
		}
		return 0;
	}

	void clear_queues();

private:
	FastQueue<LightNode> _block_propagate_queue;
	FastQueue<LightNode> _block_remove_queue;

	FastQueue<LightNode> _sun_propagate_queue;
	FastQueue<LightNode> _sun_remove_queue;
};

} // namespace zylann::voxel

#endif // VOXEL_LIGHT_ENGINE_H
