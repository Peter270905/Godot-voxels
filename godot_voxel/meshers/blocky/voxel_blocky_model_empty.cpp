#include "voxel_blocky_model_empty.h"
#include "blocky_model_baking_context.h"

namespace zylann::voxel {

VoxelBlockyModelEmpty::VoxelBlockyModelEmpty() {
	set_collision_aabbs(Span<const AABB>());
	set_light_filter(0);
	set_light_emission(0);
}

void VoxelBlockyModelEmpty::bake(blocky::ModelBakingContext &ctx) const {
	ctx.model.clear();
	VoxelBlockyModel::bake(ctx);
}

Ref<Mesh> VoxelBlockyModelEmpty::get_preview_mesh() const {
	return Ref<Mesh>();
}

bool VoxelBlockyModelEmpty::is_empty() const {
	return true;
}

} // namespace zylann::voxel
