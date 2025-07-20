#pragma once

#include <array>
#include <vector>
#include <cstddef>
#include <map>
#include <mutex>

#include <Emu/RSX/RSXThread.h>
#include <Emu/RSX/Common/TextureUtils.h>
#include <util/linalg_stuff.hpp>

struct sampled_image_descriptor;

struct mesh_draw_dump_block
{
	std::vector<u8> vertex_data;
	rsx::interleaved_range_info interleaved_range_info;
};

using tex_raw_data_ptr_t = std::vector<std::byte>*;

struct mesh_draw_dump
{
	u32 index;
	u32 clear_count; // Number of clears since frame start when this draw was dumped
	//std::vector<mesh_draw_vertex> vertices;
	//std::vector<u8> vertex_data;
	std::vector<tex_raw_data_ptr_t> texture_raw_data_ptrs;
	std::vector<u8> volatile_data;
	std::vector<mesh_draw_dump_block> blocks;
	std::vector<u32> indices;
	std::array<float4, 468> vertex_constants_buffer;
	std::vector<vec4> fragment_constants_buffer;
	std::vector<usz> fragment_constants_offsets;
	u32 vert_shader_hash;
	u32 frag_shader_hash;
	u32 transform_branch_bits;
};

struct texture_info_t
{
	u32 width;
	u32 height;
	u8 format; // CELL_GCM_TEXTURE_*
	bool is_used;
	bool is_opaque;
};

// key is tex_raw_data_ptr_t (can't actually use it)
extern std::map<u64, texture_info_t> g_dump_texture_info;

struct mesh_dumper
{
	std::vector<mesh_draw_dump> dumps;

	bool enabled{};
	bool enable_this_frame{};
	bool enable_this_frame2{};

	void push_block(const mesh_draw_dump_block& block);
	void push_dump(mesh_draw_dump& dump);
	mesh_draw_dump& get_prev_dump();
	mesh_draw_dump& get_dump();
	void dump();

	template <typename SampledImageDescriptorT>
	void save_texture(SampledImageDescriptorT* sampler_state, u32 idx, const rsx::fragment_texture& tex)
	{
		if (sampler_state->upload_context == rsx::texture_upload_context::shader_read &&
			sampler_state->image_type == rsx::texture_dimension_extended::texture_dimension_2d &&
			sampler_state->format_class == rsx::format_class::RSX_FORMAT_CLASS_COLOR)
		{
			auto& dump                    = get_dump();
			dump.texture_raw_data_ptrs.push_back(&sampler_state->image_handle->image()->raw_data);

			texture_info_t tex_info{};
			tex_info.width                                          = tex.width();
			tex_info.height                                         = tex.height();
			tex_info.format                                         = tex.format();
			g_dump_texture_info[(u64)dump.texture_raw_data_ptrs.back()] = tex_info;
		}
	}
};

extern u32 g_clears_this_frame;

extern mesh_dumper g_mesh_dumper;
extern std::mutex g_mesh_dumper_mtx;

extern bool g_mesh_dump_config_posed;