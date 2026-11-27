#ifndef VOXEL_BLOCKY_BAKED_LIBRARY_H
#define VOXEL_BLOCKY_BAKED_LIBRARY_H

#include "../../constants/cube_tables.h"
#include "../../util/containers/dynamic_bitset.h"
#include "../../util/containers/fixed_array.h"
#include "../../util/containers/std_unordered_map.h"
#include "../../util/containers/std_vector.h"
#include "../../util/godot/core/aabb.h"
#include "../../util/math/color.h"
#include "../../util/math/funcs.h"
#include "../../util/math/vector2f.h"
#include "../../util/math/vector3f.h"
#include "../../util/math/vector3i.h"

// This is data directly consumed by the blocky mesher.

namespace zylann::voxel::blocky {

// Limit based on maximum supported by VoxelMesherBlocky.
// Supporting more requires to double the size of voxels (32-bit), but it's a suspicious situation. Minecraft block
// states don't even reach a quarter of that limit. Needing more sounds like it's not the
// right approach.
static constexpr unsigned int MAX_MODELS = 65536;
// Limit related to how we index baked fluids
static constexpr unsigned int MAX_FLUIDS = 256;

// Materials must be kept to a minimum. 256 is already a lot, but that only affects performance. This limit is
// the one beyond which the code stops working. Could still be increased in theory (requires some code changes to
// use uint32_t instead of uint16_t), but there is no practical sense doing so.
static constexpr unsigned int MAX_MATERIALS = 65536;

// Convention to mean "nothing".
// Don't assign a non-empty model at this index.
static const uint16_t AIR_ID = 0;
static const uint8_t NULL_FLUID_INDEX = 255;
static constexpr uint32_t MAX_SURFACES = 2;

// Plain data strictly used by the mesher.
// It becomes distinct because it's going to be used in a multithread environment,
// while the configuration that produced the data can be changed by the user at any time.
// Also, it is lighter than Godot resources.
struct BakedModel {
	struct SideSurface {
		StdVector<Vector3f> positions;
		StdVector<Vector2f> uvs;
		StdVector<int> indices;
		StdVector<float> tangents;
		// Normals aren't stored because they are assumed to be the same for the whole side

		void clear() {
			positions.clear();
			uvs.clear();
			indices.clear();
			tangents.clear();
		}
	};

	struct Surface {
		// Inside part of the model.
		StdVector<Vector3f> positions;
		StdVector<Vector3f> normals;
		StdVector<Vector2f> uvs;
		StdVector<int> indices;
		StdVector<float> tangents;

		uint32_t material_id = 0;
		bool collision_enabled = true;

		void clear() {
			positions.clear();
			normals.clear();
			uvs.clear();
			indices.clear();
			tangents.clear();
		}
	};

	struct Model {
		// A model can have up to 2 materials.
		// If more is needed or profiling tells better, we could change it to a vector?
		FixedArray<Surface, MAX_SURFACES> surfaces;
		// Model sides: they are separated because this way we can occlude them easily.
		FixedArray<FixedArray<SideSurface, MAX_SURFACES>, Cube::SIDE_COUNT> sides_surfaces;
		unsigned int surface_count = 0;
		// Cached information to check this case early.
		// Bits are indexed with the Cube::Side enum.
		uint8_t empty_sides_mask = 0;
		uint8_t full_sides_mask = 0;

		// Tells what is the "shape" of each side in order to cull them quickly when in contact with neighbors.
		// Side patterns are still determined based on a combination of all surfaces.
		FixedArray<uint32_t, Cube::SIDE_COUNT> side_pattern_indices;
		// Side culling is all or nothing.
		// If we want to support partial culling with baked models (needed if you do fluids with "staircase"
		// models), we would need another lookup table that given two side patterns, outputs alternate geometry data
		// that is pre-cut. This would require a lot more data and precomputations though, and the cases in
		// which this is needed could make use of different approaches such as procedural generation of the
		// geometry.

		// [side][neighbor_shape_id] => pre-cut SideSurfaces
		// Surface to attempt using when a side passes the visibility test and cutout is enabled.
		// If the SideSurface from this container is empty or not found, fallback on full surface
		FixedArray<StdUnorderedMap<uint32_t, FixedArray<SideSurface, MAX_SURFACES>>, Cube::SIDE_COUNT>
				cutout_side_surfaces;
		// TODO ^ Make it UniquePtr? That array takes space for what is essentially a niche feature

		void clear() {
			for (Surface &surface : surfaces) {
				surface.clear();
			}
			for (FixedArray<SideSurface, MAX_SURFACES> &side_surfaces : sides_surfaces) {
				for (SideSurface &side_surface : side_surfaces) {
					side_surface.clear();
				}
			}
		}
	};

	Model model;
	Color color;
	uint8_t transparency_index;
	bool culls_neighbors;
	bool contributes_to_ao;
	bool empty = true;
	bool is_random_tickable;
	bool is_transparent;
	bool cutout_sides_enabled = false;
	uint8_t fluid_index = NULL_FLUID_INDEX;
	uint8_t fluid_level;
	bool lod_skirts;

	uint32_t box_collision_mask;
	uint32_t tags_mask;
	StdVector<AABB> box_collision_aabbs;

	uint8_t light_emission = 0;
	uint8_t light_filter = 15;

	bool has_variants = false;
	FixedArray<uint32_t, 16> variant_baked_indices;

	BakedModel() {
		for (uint32_t &idx : variant_baked_indices) {
			idx = UINT32_MAX;
		}
	}

	inline void clear() {
		model.clear();
		empty = true;
		light_emission = 0;
		light_filter = 1;
		has_variants = false;
		for (uint32_t &idx : variant_baked_indices) {
			idx = UINT32_MAX;
		}
	}
};

struct BakedFluid {
	static constexpr float TOP_HEIGHT = 0.9375f;
	static constexpr float BOTTOM_HEIGHT = 0.0625f;

	struct Surface {
		StdVector<Vector3f> positions;
		StdVector<int> indices;
		StdVector<float> tangents;
		// Normals aren't stored because they are assumed to be the same for the whole side
		// There are no UVs because they are generated for animation

		void clear() {
			positions.clear();
			indices.clear();
			tangents.clear();
		}
	};

	FixedArray<Surface, Cube::SIDE_COUNT> side_surfaces;

	// StdVector<uint16_t> level_model_indices;
	uint32_t material_id = 0;
	uint8_t max_level = 1;
	bool dip_when_flowing_down = false;
	// uint32_t box_collision_mask = 0;
};

struct BakedLibrary {
	// 2D array: { X : pattern A, Y : pattern B } => Does A occlude B
	// Where index is X + Y * pattern count
	DynamicBitset side_pattern_culling;
	unsigned int side_pattern_count = 0;
	// Lots of data can get moved but it's only on load.
	StdVector<BakedModel> models;
	StdVector<BakedModel> extra_variant_models;
	StdVector<BakedFluid> fluids;

	// struct VariantInfo {
	// 	uint16_t type_index;
	// 	FixedArray<uint8_t, 4> attributes;
	// };

	// StdVector<VariantInfo> variant_infos;

	unsigned int indexed_materials_count = 0;

	Vector3i collision_box_extra_min = Vector3i(0, 0, 0);
	Vector3i collision_box_extra_max = Vector3i(0, 0, 0);
	bool has_collision_box_protrusions = false;

	inline bool has_model(uint32_t i) const {
		return i < models.size();
	}

	inline const BakedModel &get_model(uint32_t type_id, uint8_t variant_id) const {
		const BakedModel &base = models[type_id];
		if (variant_id == 0 || !base.has_variants) {
			return base;
		}
		const uint8_t idx = variant_id & 0x0F;
		const uint32_t extra_idx = base.variant_baked_indices[idx];
		if (extra_idx != UINT32_MAX && extra_idx < extra_variant_models.size()) {
			return extra_variant_models[extra_idx];
		}
		return base;
	}

	inline bool get_side_pattern_occlusion(unsigned int pattern_a, unsigned int pattern_b) const {
#ifdef DEBUG_ENABLED
		CRASH_COND(pattern_a >= side_pattern_count);
		CRASH_COND(pattern_b >= side_pattern_count);
#endif
		return side_pattern_culling.get(pattern_a + pattern_b * side_pattern_count);
	}

	void find_collision_box_protrusions() {
		collision_box_extra_min = Vector3i(0, 0, 0);
		collision_box_extra_max = Vector3i(0, 0, 0);
		has_collision_box_protrusions = false;

		float min_x = 0.0f;
		float min_y = 0.0f;
		float min_z = 0.0f;
		float max_x = 1.0f;
		float max_y = 1.0f;
		float max_z = 1.0f;

		constexpr float EPSILON = 0.0001f;

		auto scan_model_aabbs = [&](const BakedModel &bm) {
			for (const AABB &aabb : bm.box_collision_aabbs) {
				const Vector3 aabb_min = aabb.position;
				const Vector3 aabb_max = aabb.position + aabb.size;

				if (aabb_min.x < min_x - EPSILON) {
					min_x = aabb_min.x;
				}
				if (aabb_min.y < min_y - EPSILON) {
					min_y = aabb_min.y;
				}
				if (aabb_min.z < min_z - EPSILON) {
					min_z = aabb_min.z;
				}

				if (aabb_max.x > max_x + EPSILON) {
					max_x = aabb_max.x;
				}
				if (aabb_max.y > max_y + EPSILON) {
					max_y = aabb_max.y;
				}
				if (aabb_max.z > max_z + EPSILON) {
					max_z = aabb_max.z;
				}
			}
		};

		for (const BakedModel &bm : models) {
			scan_model_aabbs(bm);
		}
		for (const BakedModel &bm : extra_variant_models) {
			scan_model_aabbs(bm);
		}

		if (min_x < -EPSILON || min_y < -EPSILON || min_z < -EPSILON ||
			max_x > 1.0f + EPSILON || max_y > 1.0f + EPSILON || max_z > 1.0f + EPSILON) {
			has_collision_box_protrusions = true;
			collision_box_extra_min = Vector3i(
				int(Math::ceil(math::max(0.0f, -min_x - EPSILON))),
				int(Math::ceil(math::max(0.0f, -min_y - EPSILON))),
				int(Math::ceil(math::max(0.0f, -min_z - EPSILON)))
			);
			collision_box_extra_max = Vector3i(
				int(Math::ceil(math::max(0.0f, max_x - 1.0f - EPSILON))),
				int(Math::ceil(math::max(0.0f, max_y - 1.0f - EPSILON))),
				int(Math::ceil(math::max(0.0f, max_z - 1.0f - EPSILON)))
			);
		}
	}
};

} // namespace zylann::voxel::blocky

#endif // VOXEL_BLOCKY_BAKED_LIBRARY_H
