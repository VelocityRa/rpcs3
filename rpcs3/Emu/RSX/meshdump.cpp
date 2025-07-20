#include "stdafx.h"
#include "meshdump.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdint>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <Emu/RSX/stb_image_write.h>
#include <Emu/RSX/s3tc.h>
#include <Emu/RSX/umHalf.h>
#include <Emu/RSX/RSXThread.h>
#include <Emu/System.h>
#include "Common/TextureUtils.h"

#define SLY_1 1
#define SLY_2 2
#define SLY_3 3
#define SLY_4 4
#define COD_BO 5
#define MESHDUMP_GAME SLY_2

#define MESHDUMP_DEBUG false
#define MESHDUMP_DEBUG_OLD false
//#define MESHDUMP_POSED false
#define MESHDUMP_NOCLIP false
#define MESHDUMP_NOCLIP_BATCH_DUMPS true
#define MESHDUMP_GENERIC_FILENAMES true
#define MESHDUMP_OVERWRITE false

//#pragma optimize("", off)

using namespace rsx;

// todo move
namespace neolib
{
	template <class Elem, class Traits>
	inline void hex_dump(const void* aData, std::size_t aLength, std::basic_ostream<Elem, Traits>& aStream, std::size_t aWidth = 16, ::std::string prepend_line = "")
	{
		const char* const start = static_cast<const char*>(aData);
		const char* const end   = start + aLength;
		const char* line        = start;
		while (line != end)
		{
			aStream << prepend_line;
			aStream.width(4);
			aStream.fill('0');
			aStream << std::hex << line - start << " ";
			std::size_t lineLength = std::min(aWidth, static_cast<std::size_t>(end - line));
			for (std::size_t pass = 1; pass <= 3; ++pass)
			{
				if (pass == 3)
				{
					const auto word_count = aWidth / 4;
					for (const char* next = line; next < line + word_count * 4; next += 4)
					{
						const auto v = _byteswap_ulong(*(const u32*)(next));
						aStream << *(const float*)&v;
						aStream << " ";
					}
					break;
				}

				for (const char* next = line; next != end && next != line + aWidth; ++next)
				{
					char ch = *next;
					switch (pass)
					{
					case 1:
						aStream << (ch < 32 ? '.' : ch);
						break;
					case 2:
						if (next != line)
							aStream << " ";
						aStream.width(2);
						aStream.fill('0');
						aStream << std::hex << std::uppercase << static_cast<int>(static_cast<unsigned char>(ch));
						break;
					}
				}
				if (pass == 1 && lineLength != aWidth)
					aStream << std::string(aWidth - lineLength, ' ');
				aStream << " ";
			}
			aStream << std::endl;
			line = line + lineLength;
		}
	}
} // namespace neolib

// key is tex_raw_data_ptr_t (can't actually use it)
std::map<u64, texture_info_t> g_dump_texture_info;
mesh_dumper g_mesh_dumper{};
std::mutex g_mesh_dumper_mtx{};
u32 g_clears_this_frame{};
extern bool g_mesh_dump_config_posed{true};

void mesh_dumper::push_block(const mesh_draw_dump_block& block)
{
	get_dump().blocks.push_back(block);
}

void mesh_dumper::push_dump(mesh_draw_dump& dump)
{
	dump.index = dumps.size() + 1;
	dump.clear_count = g_clears_this_frame;
	dumps.push_back(dump);
}

mesh_draw_dump& mesh_dumper::get_prev_dump()
{
	return dumps.at(dumps.size() - 2);
}

mesh_draw_dump& mesh_dumper::get_dump()
{
	return dumps.back();
}

void mesh_dumper::dump()
{
	Emu.Pause();

	static u32 dump_index{};

	static int session_id = time(NULL) & 0xFFFFFF;

#if MESHDUMP_GENERIC_FILENAMES

	const std::string dump_dir = fmt::format("meshdump_%X", session_id);
	const std::string dump_file   = fmt::format("meshdump_%X_%d", session_id, dump_index);

	const std::string tex_dir_rel = fmt::format("textures_%d", dump_index);

#elif MESHDUMP_GAME == SLY_3

	const auto map_name         = std::string((char*)vm::base(0x00788B4C));
	const bool is_job           = (vm::read32(0x005EB494) != -1u);
	const u32 world_id          = vm::read32(0x006CC750);
	std::string map_details;
	if (is_job)
		map_details = fmt::format("job%d", vm::read32(0x005EB484));
	else
		map_details = "hub";
	const std::string dump_dir  = fmt::format("C:/Code/cpp/noclip.website/data/SlyDump/" "sly3_%d_%s_%s", world_id, map_name, map_details);

	u32 next_dump_index = 0;

#if !MESHDUMP_OVERWRITE
	if (std::filesystem::exists(dump_dir))
	{
		for (const auto& entry : std::filesystem::directory_iterator(dump_dir))
			if (entry.is_regular_file())
				next_dump_index++;
	}
	else
	{
		std::filesystem::create_directory(dump_dir);
	}
#endif

	const std::string dump_file   = fmt::format("%d", next_dump_index);
	const std::string tex_dir_rel = fmt::format("%d", next_dump_index);

#else
	#error unimplemented game
#endif

	const std::string tex_dir     = fmt::format("%s/%s", dump_dir, tex_dir_rel);

#if MESHDUMP_OVERWRITE
	std::filesystem::remove_all(dump_dir);
	std::filesystem::create_directory(dump_dir);
#endif

	std::filesystem::create_directories(tex_dir);

	//
	// DUMP MATERIALS
	//

	// TODO(?): hash-based texture storage/indexing

	for (auto dump_idx = 0; dump_idx < g_mesh_dumper.dumps.size(); ++dump_idx)
	{
		auto& d = g_mesh_dumper.dumps[dump_idx];

		for (const auto& texture_raw_data_ptr : d.texture_raw_data_ptrs)
		{
			if ((u64)texture_raw_data_ptr != 0)
				g_dump_texture_info[(u64)texture_raw_data_ptr].is_used = true;
		}
	}

	// TODO: some textures on OpenGL are broken

	const std::string mtl_file_name = fmt::format("%s/%s.mtl", dump_dir.c_str(), dump_file);

#if !MESHDUMP_NOCLIP
	std::ofstream file_mtl(mtl_file_name);
#endif

	std::string mtl_str;

	std::vector<u8> work_buf;
	std::vector<u8> final_data;
	for (auto& [raw_data_ptr, info_] : g_dump_texture_info)
	{
		if (!info_.is_used)
			continue;

		const std::string tex_file_name     = fmt::format("%s/0x%X.png", tex_dir, (u64)raw_data_ptr);
		const std::string tex_file_name_rel = fmt::format("%s/0x%X.png", tex_dir_rel, (u64)raw_data_ptr);

		const auto src = (const unsigned char*)((tex_raw_data_ptr_t)raw_data_ptr)->data();

		if (src == 0 || (u64)src == 0xf || ((u64)src & 0x8000000000000000)) // hacky af
		{
			mtl_str += fmt::format("# warning: skipped texture, src is 0\n");
			continue;
		}

		final_data.resize(info_.width * info_.height * 4);

		bool is_fully_opaque = true;

		if (info_.format == CELL_GCM_TEXTURE_COMPRESSED_DXT45)
		{
			BlockDecompressImageDXT5(info_.width, info_.height, src, (unsigned long*)final_data.data());
			for (auto i = 0; i < final_data.size() / 4; ++i)
			{
				u32* ptr    = &((u32*)final_data.data())[i];
				const u32 v = *ptr;
				if ((v & 0x000000FF) == 0x00000080)
				{
					*ptr = (((v & 0xFF000000) >> 24) |
							((v & 0x00FF0000) >> 8) |
							((v & 0x0000FF00) << 8) |
							0xFF000000);
				}
				else
				{
					*ptr = (((v & 0xFF000000) >> 24) |
                            ((v & 0x00FF0000) >> 8) |
                            ((v & 0x0000FF00) << 8) |
                           (((v & 0x000000FF) * 2) << 24));
					is_fully_opaque = false;
				}
			}
		}
		else if (info_.format == CELL_GCM_TEXTURE_COMPRESSED_DXT1) // Sly 4
		{
			BlockDecompressImageDXT1(info_.width, info_.height, src, (unsigned long*)final_data.data());
			for (auto i = 0; i < final_data.size() / 4; ++i)
			{
				u32* ptr    = &((u32*)final_data.data())[i];
				const u32 v = *ptr;
				if ((v & 0x000000FF) != 0x000000FF)
				{
					is_fully_opaque = false;
				}
				*ptr = (((v & 0xFF000000) >> 24) |
						((v & 0x00FF0000) >> 8) |
						((v & 0x0000FF00) << 8) |
				       (((v & 0x000000FF) * 2) << 24));
			}
		}
		else if ((info_.format == (CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_LN)) ||
				 (info_.format == (CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_UN)))
		{
			memcpy(final_data.data(), src, info_.width * info_.height * 4);

			for (auto i = 0; i < final_data.size() / 4; ++i)
			{
				u32* ptr    = &((u32*)final_data.data())[i];
#if 0
				*ptr        = _byteswap_ulong(*ptr);
#endif
				const u32 v = *ptr;
				//is_fully_opaque &= ((v & 0xFF000000) == 0x80000000);
				if ((v & 0x000000FF) == 0x00000080)
				{
					*ptr = (((v & 0xFF000000) >> 8) |
							((v & 0x00FF0000) >> 8) |
							((v & 0x0000FF00) >> 8) |
							0xFF000000);
				}
				else
				{
					*ptr = (((v & 0xFF000000) >> 8) |
                            ((v & 0x00FF0000) >> 8) |
                            ((v & 0x0000FF00) >> 8) |
                           (((v & 0x000000FF) * 2) << 24));
					is_fully_opaque = false;
				}
			}
		}
		else
		{
			mtl_str += fmt::format("# warning: skipped texture, format is 0x%X\n", info_.format);
		}

		// argb -> rgba
		// also a sly specific thing, it's PS2 port, and GS's RGBA reg alpha is weird like that

		mtl_str += fmt::format("newmtl 0x%X\n", (u64)raw_data_ptr);
		mtl_str += fmt::format("map_Kd %s\n", tex_file_name_rel.c_str());
		mtl_str += fmt::format("Ns 10\n"); // TODO: check
		if (!is_fully_opaque)
			mtl_str += fmt::format("map_D %s\n", tex_file_name_rel.c_str());

		work_buf.resize(final_data.size());
		// if (g_cfg.video.renderer == video_renderer::vulkan)
#if MESHDUMP_NOCLIP
		memcpy(work_buf.data(), final_data.data(), final_data.size());
#else
		// copy flipped on Y
		const auto stride = info_.width * 4;
		for (auto i = 0; i < info_.height; i++)
		{
			memcpy(work_buf.data() + i * stride, final_data.data() + (final_data.size() - ((i + 1) * stride)), stride);
		}
#endif
	
		info_.is_opaque = is_fully_opaque;

		if (!stbi_write_png(tex_file_name.c_str(), info_.width, info_.height, 4, work_buf.data(), info_.width * 4))
			__debugbreak();
	}

#if MESHDUMP_NOCLIP
	// Dummy texture
	work_buf.resize(4);
	work_buf[0] = 0x00;
	work_buf[1] = 0x00;
	work_buf[2] = 0x00;
	work_buf[3] = 0x00;
	const std::string tex_file_name = fmt::format("%s/0x0.png", tex_dir);
	if (!stbi_write_png(tex_file_name.c_str(), 1, 1, 4, work_buf.data(), 1 * 4))
		__debugbreak();
#endif

#if !MESHDUMP_NOCLIP
	file_mtl.write(mtl_str.c_str(), mtl_str.size());
#endif
	
	//
	// DUMP MESHES
	//

	std::ofstream file_obj(fmt::format("%s/%s.obj", dump_dir.c_str(), dump_file));
	std::string obj_str;

	obj_str += fmt::format("mtllib %s.mtl\n", dump_file);

	u32 vertex_index_base{1};
#if MESHDUMP_NOCLIP
	vertex_index_base = 0;
#endif
	u32 vertex_index_base_normal_offset{};

	//g_mesh_dumper.dumps[4].vertex_constants_buffer

	mat4 inv_view_mat = linalg::identity;
#if MESHDUMP_GAME == SLY_3
	auto& v      = inv_view_mat;
	auto m       = (be_t<float>*)vm::base(0x007DA920);
	int i        = 0;
	v[0][0] = m[i++]; v[0][1] = m[i++]; v[0][2] = m[i++]; v[0][3] = m[i++];
	v[1][0] = m[i++]; v[1][1] = m[i++]; v[1][2] = m[i++]; v[1][3] = m[i++];
	v[2][0] = m[i++]; v[2][1] = m[i++]; v[2][2] = m[i++]; v[2][3] = m[i++];
	v[3][0] = m[i++]; v[3][1] = m[i++]; v[3][2] = m[i++]; v[3][3] = m[i++];
	inv_view_mat = linalg::inverse(inv_view_mat);
#endif

	//std::map<u32, u32> hmm;
	//for (auto dump_idx = 0; dump_idx < g_mesh_dumper.dumps.size(); ++dump_idx)
	//{
	//	auto& d = g_mesh_dumper.dumps[dump_idx];
	//	hmm[(u32)d.texture_raw_data_ptr]++;
	//}

#if MESHDUMP_NOCLIP && MESHDUMP_NOCLIP_BATCH_DUMPS
	// todo figure out how to keep opaque ones first?

	std::stable_sort(g_mesh_dumper.dumps.begin(), g_mesh_dumper.dumps.end(), [](const mesh_draw_dump& d0, const mesh_draw_dump& d1) {
		// if (g_dump_texture_info[(u64)d1.texture_raw_data_ptr].is_opaque)
		//	  return true;
		//const bool shaders_same = (d0.vert_shader_hash == d1.vert_shader_hash && d0.frag_shader_hash == d1.frag_shader_hash);
		//if (!shaders_same)
			//return d0.vert_shader_hash < d1.vert_shader_hash;
		//else
		return (u64)d0.texture_raw_data_ptr < (u64)d1.texture_raw_data_ptr;
	});
#endif
	bool new_dump = true;

	for (auto dump_idx = 0; dump_idx < g_mesh_dumper.dumps.size(); ++dump_idx)
	{
		auto& d = g_mesh_dumper.dumps[dump_idx];

		const auto block_count = d.blocks.size();
		const auto index_count = d.indices.size();

		if (block_count == 0 || index_count == 0)
			continue;


		// todo put this logic in the earlier in the is_used thing so that we don't dump unused textures
		const u32 tex_count = d.texture_raw_data_ptrs.size();
		u64 tex_raw_idx = 0;
#if MESHDUMP_GAME == SLY_1
		if (tex_count == 3)
			tex_raw_idx = 1;
#endif

		const tex_raw_data_ptr_t texture_raw_data_ptr = tex_count > 0 ? d.texture_raw_data_ptrs[tex_raw_idx] : nullptr;

		const bool last_dump = (dump_idx == g_mesh_dumper.dumps.size() - 1);

		const auto dump_name = fmt::format("%d_%X_vshd:%08X_fshd:%08X_tex:%X_clr:%d_blk:%d",
			d.index, session_id, d.vert_shader_hash, d.frag_shader_hash, (u32)texture_raw_data_ptr, d.clear_count, d.blocks.size());

		// Skip if dump is identical to previous (Sly does this for some reason, one draw writes to depth, the other not)
		if (dump_idx > 0)
		{
			const auto& d_prev = g_mesh_dumper.dumps[dump_idx - 1];

			bool is_identical_dump = true;
			if (d.vert_shader_hash != d_prev.vert_shader_hash ||
				d.frag_shader_hash != d_prev.frag_shader_hash ||
				d.blocks.size() != d_prev.blocks.size())
			{
				is_identical_dump = false;
			}
			else
			{
				if (d.indices.size() != d_prev.indices.size())
				{
					is_identical_dump = false;
				}
				else
				{
					auto rebase_indices = [](const std::vector<u32>& v) -> std::vector<u32> {
						const auto v_base = *std::min_element(v.begin(), v.end());
						auto v_rebased    = v;
						for (auto& i : v_rebased)
							i -= v_base;
						return v_rebased;
					};

					auto d_rebased = rebase_indices(d.indices);
					auto d_prev_rebased = rebase_indices(d_prev.indices);

					if (memcmp(d_rebased.data(), d_prev_rebased.data(), d_rebased.size() * 4))
					{
						is_identical_dump = false;
					}
					else
					{
						for (int i = 0; i < d.blocks.size(); ++i)
						{
							const auto& b0 = d.blocks[i];
							const auto& b1 = d_prev.blocks[i];

							if ((b0.vertex_data.size() != b1.vertex_data.size()) ||
								memcmp(b0.vertex_data.data(), b1.vertex_data.data(), b0.vertex_data.size()))
							{
								is_identical_dump = false;
								break;
							}
						}
						if (is_identical_dump)
						{
							if (memcmp(d.vertex_constants_buffer.data(), d_prev.vertex_constants_buffer.data(), d.vertex_constants_buffer.size() * 16))
							{
								is_identical_dump = false;
							}
						}
					}
				}
			}
			if (is_identical_dump)
			{
#if MESHDUMP_DEBUG
				obj_str += fmt::format("# skipping identical dump: %s\n", dump_name);
#endif
				continue;
			}
		}

#if MESHDUMP_NOCLIP && MESHDUMP_NOCLIP_BATCH_DUMPS
		if (dump_idx > 0)
		{
			const auto& d_prev = g_mesh_dumper.dumps[dump_idx - 1];
			// force new drawcall if shaders are different
			new_dump = (d.vert_shader_hash != d_prev.vert_shader_hash || d.frag_shader_hash != d_prev.frag_shader_hash);
			// force new drawcall if texture is different
			new_dump = new_dump || ((u64)texture_raw_data_ptr != (u64)d_prev.texture_raw_data_ptr);
			// force new drawcall if texture has transparency to reduce visual bugs
			new_dump = new_dump || !g_dump_texture_info[(u64)texture_raw_data_ptr].is_opaque;
#if MESHDUMP_GAME == SLY_3
			// force new draw call if skeletal shader because of reasons
			new_dump = new_dump || (d.vert_shader_hash == 0xAB2CD1A9);
#endif
		}
#endif

		obj_str += fmt::format("%s %s\n", new_dump ? "o" : "g", dump_name);

#if MESHDUMP_DEBUG
		if (auto it = g_dump_texture_info.find((u64)texture_raw_data_ptr); it != g_dump_texture_info.end())
		{
			obj_str += fmt::format("# Texture %dx%d fmt 0x%X\n", it->second.width, it->second.height, it->second.format);
		}
#endif
		//if (g_dump_texture_info.contains((u64)texture_raw_data_ptr))
		//{
		//	const auto src = (const unsigned char*)((tex_raw_data_ptr_t)texture_raw_data_ptr)->data();

		//	if (src == 0 || (u64)src == 0xf || ((u64)src & 0x8000000000000000)) { // hacky af
		//		obj_str += fmt::format("usemtl 0x0\n");
		//	}
		//	else
		//	{
		//		obj_str += fmt::format("usemtl 0x%X\n", texture_raw_data_ptr);
		//	}
		//}
		//else
		//{
		//	obj_str += fmt::format("usemtl 0x0\n");
		//}
#if MESHDUMP_NOCLIP
		if (new_dump)
			obj_str += fmt::format("usemtl 0x%X %d\n",
				texture_raw_data_ptr, g_dump_texture_info[(u64)texture_raw_data_ptr].is_opaque);
#else
		obj_str += fmt::format("usemtl 0x%X\n", texture_raw_data_ptr);
#endif

		//auto dst_ptr = d.vertex_data.data();
		//std::stringstream hex_ss;
		//neolib::hex_dump((const void*)dst_ptr, d.vertex_data.size(), hex_ss, 36);
		//auto hex_str = hex_ss.str();
		////hex_str.pop_back(); // remove newline
		//obj_str += hex_str;

#if MESHDUMP_DEBUG
		for (int i = 0; i < d.blocks.size(); ++i)
		{
			std::string hex_str;
			const auto& block                    = d.blocks[i];
			auto interleaved_attribute_array_str = [](rsx::simple_array<interleaved_attribute_t> locations) -> std::string {
				std::string locs_str = "{ ";
				for (const auto& l : locations)
				{
					if (l.modulo == 0 && l.frequency == 0)
						locs_str += fmt::format("%d,", l.index);
					else
						locs_str += fmt::format("%d(%d,%d),", l.index, l.modulo, l.frequency);
				}
				locs_str.erase(locs_str.size() - 1, 1);
				locs_str += " }";
				return locs_str;
			};
			hex_str += fmt::format("# Block %d, %s, locs: %s, count: %d, size: %d\n",
				i, block.interleaved_range_info.to_str().c_str(),
				interleaved_attribute_array_str(block.interleaved_range_info.locations),
				block.vertex_data.size() / block.interleaved_range_info.attribute_stride, block.vertex_data.size());
			{
				auto dst_ptr = block.vertex_data.data();
				std::stringstream hex_ss;
				auto hex_width = block.interleaved_range_info.attribute_stride;
				if (block.interleaved_range_info.attribute_stride <= 4 &&
					((d.blocks[i].vertex_data.size() % 64) == 0))
					hex_width = 64;
				neolib::hex_dump((const void*)dst_ptr, d.blocks[i].vertex_data.size(), hex_ss, hex_width, " # ");
				hex_str += hex_ss.str();
				//hex_str.pop_back(); // remove newline
			}
			obj_str += hex_str;
		}

		if (!d.volatile_data.empty())
		{
			obj_str += fmt::format("# Volatile Data size 0x%X\n", d.volatile_data.size());
			auto dst_ptr = d.volatile_data.data();
			std::stringstream hex_ss;
			neolib::hex_dump((const void*)dst_ptr, d.volatile_data.size(), hex_ss, 32, " # ");
			auto hex_str = hex_ss.str();
			//hex_str.pop_back(); // remove newline
			obj_str += hex_str;
		}
#endif

		// TODO: move to structs

		//auto dot = [](const float4& v0, const float4& v1) -> float {
		//	return v0.x * v1.x + v0.y * v1.y + v0.z * v1.z + v0.w * v1.w;
		//};

		auto mul_inv = [&](const float4& v0, const mat4& v1) -> float4
		{
			return {
				dot(v0, v1[0]),
				dot(v0, v1[1]),
				dot(v0, v1[2]),
				dot(v0, v1[3])};
		};

		auto print_vec4 = [](float4 v) {
			return fmt::format("# { %5.2f, %5.2f, %5.2f, %5.2f }", v.x, v.y, v.z, v.w);
		};

		auto print_mat43 = [](float4 _0, float4 _1, float4 _2) {
			return fmt::format("# ((( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f )))",
				_0.x, _1.x, _2.x, 0.0,
				_0.y, _1.y, _2.y, 0.0,
				_0.z, _1.z, _2.z, 0.0,
				_0.w, _1.w, _2.w, 1.0);
		};

		auto print_mat4 = [](float4 _0, float4 _1, float4 _2, float4 _3) {
			return fmt::format("# ((( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f ),\n#   ( %5.2f, %5.2f, %5.2f, %5.2f )))",
				_0.x, _1.x, _2.x, _3.x,
				_0.y, _1.y, _2.y, _3.y,
				_0.z, _1.z, _2.z, _3.z,
				_0.w, _1.w, _2.w, _3.w);
		};

		auto print_mat4_ = [&](mat4 m) {
			return print_mat4(m[0], m[1], m[2], m[3]);
		};

		const auto& vcb = d.vertex_constants_buffer;

#if MESHDUMP_GAME < SLY_4
		// TODO: would be cleaner to base things on just transform bits and not block count, also simplify related logic

		enum class draw_type_e
		{
			sly1_normal,
			sly1_skeletal,
			sly1_particle,
			sly1_particle2,
			sly1_skydome1,
			sly1_skydome2,
			sly1_skydome3,
			sly1_skeletal2,
			sly1_unk_32,
			sly1_unk_36,

			sly3_normal,
			sly3_skydome,
			sly3_skydome2,
			sly3_nospec,
			sly3_water,    // Sly3_RenderList_2_Lit_Skin_V.vpo and Sly3_RenderList_2_Lit_Skin.fpo
			sly3_skeletal, // ditto
			sly3_normal2,   // 
			sly3_particle,  // fire?
			sly3_particle2, // embers?
			sly3_particle3, // bubbles?

			invalid = -1,
		} draw_type = draw_type_e::invalid;

		u8 stride0 = 0;
		if (!d.blocks.empty())
			stride0 = d.blocks[0].interleaved_range_info.attribute_stride;

		if (MESHDUMP_GAME == SLY_1)
		{
			static bool has_skydome1 = false;
			if (last_dump)
				has_skydome1 = false; // reset;

			     if (d.vert_shader_hash == 0xC3E6087F && d.frag_shader_hash == 0x7799DB4E) draw_type = draw_type_e::sly1_normal;
			else if (d.vert_shader_hash == 0x97965BB7 && d.frag_shader_hash == 0x7799DB4E) draw_type = draw_type_e::sly1_normal; // skeletal
			else if (d.vert_shader_hash == 0xC4AF92E3 && d.frag_shader_hash == 0xE521BA10) {
					 if (tex_count == 1) {
						      if (block_count == 2 && !has_skydome1) { draw_type = draw_type_e::sly1_skydome1; has_skydome1 = true; }
						 else if (stride0 == 36)                       draw_type = draw_type_e::sly1_skydome3;
						 else										   draw_type = draw_type_e::sly1_skydome2; }
					 else draw_type = draw_type_e::sly1_particle; }
			else if (d.vert_shader_hash == 0x6E50F33B && d.frag_shader_hash == 0xAF6821E4) draw_type = draw_type_e::sly1_skeletal2;
			else if (d.vert_shader_hash == 0x5346134B && d.frag_shader_hash == 0xAF6821E4) draw_type = draw_type_e::sly1_unk_36;
			else if (d.vert_shader_hash == 0xB3D613CF && d.frag_shader_hash == 0x5AA5D1FC) draw_type = draw_type_e::sly1_unk_36;
			else if (d.vert_shader_hash == 0x460476E1 && d.frag_shader_hash == 0x5AA5D1FC) draw_type = draw_type_e::sly1_unk_36;
			else if (d.vert_shader_hash == 0xE2310449 && d.frag_shader_hash == 0x76A79373) draw_type = draw_type_e::sly1_particle2;
			else if (d.vert_shader_hash == 0x31C70E3B && d.frag_shader_hash == 0x76A79373) draw_type = draw_type_e::sly1_particle2;
		}
		else if ((MESHDUMP_GAME == SLY_2) || (MESHDUMP_GAME == SLY_3))
		{
			if (MESHDUMP_GAME == SLY_3 && d.clear_count == 1)
				 if (stride0 == 28) draw_type = draw_type_e::sly3_skydome2;
					 else draw_type = draw_type_e::sly3_skydome;
			else if (d.vert_shader_hash == 0x47DA8B28 && d.frag_shader_hash == 0x0BBE0FF4) draw_type = draw_type_e::sly3_normal;
			else if (d.vert_shader_hash == 0x73783E15 && d.frag_shader_hash == 0xFFA26447) draw_type = draw_type_e::sly3_nospec;
			else if (d.vert_shader_hash == 0xAB2CD1A9 && d.frag_shader_hash == 0x0BBE0FF4 && block_count == 3) draw_type = draw_type_e::sly3_water;
			else if (d.vert_shader_hash == 0xAB2CD1A9 && d.frag_shader_hash == 0x0BBE0FF4 && block_count == 4) draw_type = draw_type_e::sly3_skeletal;
			else if (d.vert_shader_hash == 0xF34D3512 && d.frag_shader_hash == 0x08956CEB) draw_type = draw_type_e::sly3_normal2;
			else if (d.vert_shader_hash == 0xE2310449 && d.frag_shader_hash == 0x76A79373) draw_type = draw_type_e::sly3_particle;
			else if (d.vert_shader_hash == 0x31C70E3B && d.frag_shader_hash == 0x76A79373) draw_type = draw_type_e::sly3_particle2;
			else if (d.vert_shader_hash == 0xE2310449 && d.frag_shader_hash == 0x76A79373) draw_type = draw_type_e::sly3_particle3;
		}

		bool use_no_xform = (
			draw_type == draw_type_e::sly1_particle ||
			draw_type == draw_type_e::sly1_particle2 ||
			draw_type == draw_type_e::sly3_normal2 ||
			draw_type == draw_type_e::sly3_particle ||
			draw_type == draw_type_e::sly3_particle2 ||
			draw_type == draw_type_e::sly3_particle3);

		const bool is_skydome = (draw_type == draw_type_e::sly1_skydome1 ||
								 draw_type == draw_type_e::sly1_skydome2 ||
								 draw_type == draw_type_e::sly1_skydome3 ||
								 draw_type == draw_type_e::sly3_skydome);

		bool is_skin_shader = (draw_type == draw_type_e::sly3_skeletal ||
							   draw_type == draw_type_e::sly3_water);
		bool is_skinned = is_skin_shader && (d.transform_branch_bits & 0x100);

		const bool is_lighting = is_skin_shader && (d.transform_branch_bits & 0x10);

		const bool use_inv_view = !is_skydome;

		const bool has_spec = (draw_type != draw_type_e::sly3_nospec && !is_skydome);

//#if !MESHDUMP_POSED
		if (!g_mesh_dump_config_posed)
		{
			is_skinned   = false;
			use_no_xform = use_no_xform || (draw_type == draw_type_e::sly3_water || draw_type == draw_type_e::sly3_skeletal);
		}

//#endif

		mat4 xform_mat  = linalg::identity;
		mat4 xform_mat0 = linalg::identity;
		mat4 bone_mats[4]{};

		if (vcb.size() >= 17)
		{
			xform_mat0 = {vcb[0], vcb[1], vcb[2], vcb[3]};
			xform_mat  = {vcb[13], vcb[14], vcb[15], vcb[16]};

			if (is_skinned)
			{
				u32 j = 32;
				bone_mats[0] = {vcb[j++], vcb[j++], vcb[j++], {0, 0, 0, 1}};
				bone_mats[1] = {vcb[j++], vcb[j++], vcb[j++], {0, 0, 0, 1}};
				bone_mats[2] = {vcb[j++], vcb[j++], vcb[j++], {0, 0, 0, 1}};
				bone_mats[3] = {vcb[j++], vcb[j++], vcb[j++], {0, 0, 0, 1}};

				#if 0
				for (auto ii = 0; ii < 4; ii++)
				{
					bone_mats[ii][0][0] = 1;
					bone_mats[ii][0][1] = 0;
					bone_mats[ii][0][2] = 0;

					bone_mats[ii][1][0] = 0;
					bone_mats[ii][1][1] = 1;
					bone_mats[ii][1][2] = 0;

					bone_mats[ii][2][0] = 0;
					bone_mats[ii][2][1] = 0;
					bone_mats[ii][2][2] = 1;
				}
				#endif
			}
		}

		vec3 pos_offset{};
		
#if MESHDUMP_GAME == SLY_3
		if (is_skydome)
		{
			//const vec3 cam_pos = vec3(inv_view_mat[3][0], inv_view_mat[3][1], inv_view_mat[3][2]);
			// xform_mat    = linalg::translation_matrix(cam_pos); // linalg::scaling_matrix(vec3(5, 5, 5));
			xform_mat = linalg::transpose(linalg::scaling_matrix(vec3(2, 2, 2)));
			//xform_mat = linalg::identity;
		}
		else
		{
			be_t<float>* cam_pos = (be_t<float>*)vm::base(0x007da8c0);
			pos_offset           = vec3(cam_pos[0], cam_pos[1], cam_pos[2]);
		}
#endif

		auto get_skeleton_r_vecs = [&](u32 idx, const mesh_draw_dump_block* weights_block) -> std::tuple<vec4, vec4, vec4>
		{
			const vec4be* weight_array = (vec4be*)weights_block->vertex_data.data();
			ensure(idx * 16 < weights_block->vertex_data.size());
			const vec4be weights_be = weight_array[idx];
			const vec4 weights      = {weights_be.x, weights_be.y, weights_be.z, weights_be.w};

			const vec4 r0 = weights.x * bone_mats[0][0] + weights.y * bone_mats[1][0] + weights.z * bone_mats[2][0] + weights.w * bone_mats[3][0];
			const vec4 r1 = weights.x * bone_mats[0][1] + weights.y * bone_mats[1][1] + weights.z * bone_mats[2][1] + weights.w * bone_mats[3][1];
			const vec4 r2 = weights.x * bone_mats[0][2] + weights.y * bone_mats[1][2] + weights.z * bone_mats[2][2] + weights.w * bone_mats[3][2];

#if MESHDUMP_DEBUG
			//obj_str += fmt::format("# weights: %f %f %f %f, rvecs:\n%s\n%s\n%s\n",
				//weights[0], weights[1], weights[2], weights[3], print_vec4(r0), print_vec4(r1), print_vec4(r2));
#endif

			return {r0, r1, r2};
		};

		float scale_mult = 1;
#if !MESHDUMP_NOCLIP
		scale_mult = 0.01;
#endif

		auto transform_pos = [&](u32 idx, const vec3be& pos, const mesh_draw_dump_block* weights_block) -> vec3 {
			vec4 out;
			vec4 a = {pos.x, pos.y, pos.z, 1};
			if (is_skinned)
			{
				const auto &[r0, r1, r2] = get_skeleton_r_vecs(idx, weights_block);

				out = {
					dot(a, r0),
					dot(a, r1),
					dot(r2, a),
					1};

				//auto isbig = [](double x)
				//{
				//	return fabs(x) > FLT_MAX;
				//};
				//if (isnan(out.x) || isnan(out.y) || isnan(out.z) || isnan(out.w) ||
				//	isinf(out.x) || isinf(out.y) || isinf(out.z) || isinf(out.w) ||
				//    isbig(out.x) || isbig(out.y) || isbig(out.z) || isbig(out.w))
				//const double sum     = weights.x + weights.y + weights.z + weights.w;
				//const double epsilon = 0.01;
				//if ((sum > 1. + epsilon) || (sum < 1. - epsilon) || isnan(sum))
				//{
				//	// TODO: don't emit these
				//	obj_str += "# bad weights, defaulting to first bone\n";
				//	out = {
				//		dot(a, bone_mats[0][0]),
				//		dot(a, bone_mats[0][1]),
				//		dot(a, bone_mats[0][2]),
				//		1};
				//}
				out = mul_inv(out, xform_mat);
			}
			else if (use_no_xform)
			// else
			{
				// out = mul_inv(a, xform_mat_first);
				//__debugbreak();
				obj_str += "# using no xform\n";
				out = a;
			}
			else
			{
				out = mul_inv(a, xform_mat);
			}
			// return {out.x * 0.01f, out.y * 0.01f, out.z * 0.01f};

#if MESHDUMP_GAME == SLY_3
			if (use_inv_view)
				out = mul_inv(out, inv_view_mat);
			// xform_mat = linalg::mul(xform_mat, inv_view_mat);
#endif
#if MESHDUMP_GAME == SLY_3
			return vec3(-out.x + pos_offset.x, out.z + pos_offset.z, out.y + pos_offset.y) * scale_mult;
#else
			return vec3(out.x + pos_offset.x, out.y + pos_offset.y, out.z + pos_offset.z) * scale_mult;
#endif
		};
#endif

		auto transform_normal = [&](u32 idx, const vec3be& normal, const mesh_draw_dump_block* weights_block) -> vec3
		{
			vec4 out;
			vec3 a = {normal.x, normal.y, normal.z};

#if MESHDUMP_GAME <= SLY_4
			if (is_skinned)
			{
				const auto& [r0, r1, r2] = get_skeleton_r_vecs(idx, weights_block);

				const auto r0_3 = vec3(r0.x, r0.y, r0.z);
				const auto r1_3 = vec3(r1.x, r1.y, r1.z);
				const auto r2_3 = vec3(r2.x, r2.y, r2.z);

				return {
					dot(r0_3, a),
					dot(r1_3, a),
					dot(r2_3, a)};
			}
			else
#endif
			{
				return {normal.x, normal.y, normal.z};
			}
		};

		size_t vertex_count = 0;

		bool has_normals{};

#if MESHDUMP_GAME < 4

		auto emit_uniforms = [&](int draw_type) {
#if MESHDUMP_NOCLIP
			// todo: emit binary
			obj_str += fmt::format("vc %d %x %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n",
				draw_type,
				d.transform_branch_bits,
				vcb[17].x, vcb[17].y, vcb[17].z, vcb[17].w,
				vcb[18].x, vcb[18].y, vcb[18].z, vcb[18].w,
				vcb[19].x, vcb[19].y, vcb[19].z, vcb[19].w,
				vcb[29].x, vcb[29].y, vcb[29].z, vcb[29].w,
				vcb[0].x, vcb[0].y, vcb[0].z, vcb[0].w,
				vcb[1].x, vcb[1].y, vcb[1].z, vcb[1].w,
				vcb[2].x, vcb[2].y, vcb[2].z, vcb[2].w,
				vcb[3].x, vcb[3].y, vcb[3].z, vcb[3].w);

			for (u32 i = 0; i < d.fragment_constants_buffer.size(); ++i)
			{
				const usz offs = d.fragment_constants_offsets[i];
				const auto& fc = d.fragment_constants_buffer[i];
				obj_str += fmt::format("fc %d %f %f %f %f\n", offs, fc.x, fc.y, fc.z, fc.w);
			}
#endif
		};
		if (block_count > 0)
		{
			const auto& block0 = d.blocks[0];
			if (is_skinned && block_count < 2)
				__debugbreak();
			const auto* block1_weights = is_skinned ? &d.blocks[1] : nullptr;

			const u8 stride = block0.interleaved_range_info.attribute_stride;

			if (block0.interleaved_range_info.interleaved)
			{
				// if (stride == 36 && block_count == 3) {
				if (draw_type == draw_type_e::sly1_particle2)
				{
					emit_uniforms((int)draw_type);

					struct mesh_draw_vertex_32
					{
						vec3be pos;
						be_t<float> unk0; // size?
						vec2be uv;
						be_t<float> unk1;
						u32 unk2;
					};
					static_assert(sizeof(mesh_draw_vertex_32) == 32);
					if (stride != 32)
						__debugbreak();

					const mesh_draw_vertex_32* vertex_data = (mesh_draw_vertex_32*)block0.vertex_data.data();
					vertex_count                           = block0.vertex_data.size() / sizeof(mesh_draw_vertex_32);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v  = vertex_data[i];
						const vec3 pos = transform_pos(i, v.pos, block1_weights);

						obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
						obj_str += fmt::format("vt %f %f\n", (float)v.uv.u, (float)v.uv.v);
					}

					vertex_index_base_normal_offset += vertex_count;
					has_normals = false;
				}
				else if (draw_type == draw_type_e::sly1_particle || draw_type == draw_type_e::sly1_skydome2 ||
					draw_type == draw_type_e::sly1_skeletal2 || draw_type == draw_type_e::sly1_unk_32)
				{
					emit_uniforms((int)draw_type);

					struct mesh_draw_vertex_32
					{
						vec3be pos;
						vec2be uv;
						u32 unk[3];
					};
					static_assert(sizeof(mesh_draw_vertex_32) == 32);
					if (stride != 32) __debugbreak(); // obj_str += fmt::format("# errordbg %d\n", stride);

					const mesh_draw_vertex_32* vertex_data = (mesh_draw_vertex_32*)block0.vertex_data.data();
					vertex_count                           = block0.vertex_data.size() / sizeof(mesh_draw_vertex_32);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v  = vertex_data[i];
						const vec3 pos = transform_pos(i, v.pos, block1_weights);

						obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
						obj_str += fmt::format("vt %f %f\n", (float)v.uv.u, (float)v.uv.v);
					}

					vertex_index_base_normal_offset += vertex_count;
					has_normals = false;
				}
				else if (draw_type == draw_type_e::sly1_normal || draw_type == draw_type_e::sly1_unk_36 ||
					     draw_type == draw_type_e::sly1_skydome1 || draw_type == draw_type_e::sly1_skydome3 ||

						 draw_type == draw_type_e::sly3_normal || draw_type == draw_type_e::sly3_skydome ||
						 draw_type == draw_type_e::sly3_nospec || draw_type == draw_type_e::sly3_skeletal ||
						 draw_type == draw_type_e::sly3_water)
				{
					emit_uniforms((int)draw_type);

					struct mesh_draw_vertex_36
					{
						vec3be pos;
						vec3be normal;
						vec2be uv;
						u32 unk0;
					};
					static_assert(sizeof(mesh_draw_vertex_36) == 36);
					// if (stride != 36) __debugbreak(); // obj_str += fmt::format("# errordbg %d\n", stride);
					 if (stride != 36) obj_str += fmt::format("# errordbg %d\n", stride);

					const mesh_draw_vertex_36* vertex_data = (mesh_draw_vertex_36*)block0.vertex_data.data();
					vertex_count                           = block0.vertex_data.size() / sizeof(mesh_draw_vertex_36);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v  = vertex_data[i];
						const vec3 pos = transform_pos(i, v.pos, block1_weights);

#if MESHDUMP_NOCLIP
						const u32 block_increment    = (draw_type == draw_type_e::sly3_skeletal) ? 1 : 0; // todo Sly 1
						u32 diff          = ((u32*)d.blocks[1 + block_increment].vertex_data.data())[i];

						auto u32_to_vec4 = [](u32 u) -> vec4
						{
							return {
								((u & 0x000000FF) >>  0) / 255.0f,
								((u & 0x0000FF00) >>  8) / 255.0f,
								((u & 0x00FF0000) >> 16) / 255.0f,
								((u & 0xFF000000) >> 24) / 255.0f,
							};
						};
						auto vec4_to_u32 = [](vec4 v) -> u32
						{
							return
								u32(v[0] * 255) << 0  |
								u32(v[1] * 255) << 8  |
								u32(v[2] * 255) << 16 |
								u32(v[3] * 255) << 24;
							;
						};

						if (has_spec)
						{
							ensure(d.blocks.size() > 2 + block_increment);
							u32 spec = ((u32*)d.blocks[2 + block_increment].vertex_data.data())[i];

							if (is_lighting)
							{
								using std::min;
								using std::max;
							
								vec4 diff_vec          = u32_to_vec4(diff);
								const vec4 in_spec_vec = u32_to_vec4(spec);
								vec4 spec_vec          = in_spec_vec;

								const vec3 normal = transform_normal(i, v.normal, block1_weights);
								// change diff and spec

								const mat4 gvs_matNormalsTrans = {vcb[48], vcb[49], vcb[50], vcb[51]};
								const mat4 gvs_matColors       = {vcb[52], vcb[53], vcb[54], vcb[55]};
								const vec4 gvs_vecOffset       = vcb[56];
								const vec4 gvs_vecMax          = vcb[57];
								const vec4 gvs_lsd          = vcb[58];
								const float gvs_vms          = vcb[59].x;

								vec4 r1 = vec4(normal.x, normal.y, normal.z, 0.0);
								vec4 r0 = mul_inv(r1, gvs_matNormalsTrans);
								r0 = r0 * r0 * r0 + r0;

								const float r4x = gvs_vms + gvs_lsd.w;
								vec4 r3 = vec4(gvs_vecOffset.x, gvs_vecOffset.y, gvs_vecOffset.z, r4x * 2.0 + gvs_vecOffset.w);
								r1 = min(r0, gvs_vecMax);
								r0 = mul_inv(r1, gvs_matColors);
								r3 += r0;
								r0.x = 2.0 - max(max(r3.x, r3.y), r3.z);
								r0.x = max(r0.x, 0.0f);
								r3   = min(r3, vec4(2.0, 2.0, 2.0, r0.x));
								r0.x -= r3.w;
								const vec3 gvs_lsd_3 = vec3(gvs_lsd.x, gvs_lsd.y, gvs_lsd.z);
								const vec3 r0_xxx          = vec3(r0.x, r0.x, r0.x);
								r0.xyz()                   = r0_xxx * gvs_lsd_3 + r3.xyz();
								const vec3 diff_www        = vec3(diff_vec.w, diff_vec.w, diff_vec.w);
								diff_vec.xyz()             = diff_www * r0.xyz();
								float r1x = (2.01575 - r0.x) / 2.0;
								spec_vec.w    = r1x;
								r1x                        = r3.w * in_spec_vec.w / r1x;
								spec_vec.xyz()             = in_spec_vec.xyz() * vec3(r1x, r1x, r1x);
								diff_vec.w                 = in_spec_vec.w;

								obj_str += fmt::format("v36sn %f %f %f %f %f %f %f %f %f %f %f %f %f\n",
									pos.x, pos.y, pos.z, (float)v.uv.u, (float)v.uv.v, diff_vec.x, diff_vec.y,
									diff_vec.z, diff_vec.w, spec_vec.x, spec_vec.y, spec_vec.z, spec_vec.w);
							}
							else
							{

								obj_str += fmt::format("v36s %f %f %f %f %f %X %X\n",
									pos.x, pos.y, pos.z, (float)v.uv.u, (float)v.uv.v, diff, spec);
							}
						}
						else
						{
							obj_str += fmt::format("v36 %f %f %f %f %f %X\n",
								pos.x, pos.y, pos.z, (float)v.uv.u, (float)v.uv.v, diff);
						}
#else
						obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
						obj_str += fmt::format("vn %f %f %f\n", (float)v.normal.z, (float)v.normal.x, (float)v.normal.y);
						obj_str += fmt::format("vt %f %f\n", (float)v.uv.u, (float)v.uv.v);
#endif
					}

#if 0
					has_normals = true;
#else
					vertex_index_base_normal_offset += vertex_count;
					has_normals = false;
#endif
				}
				else if (draw_type == draw_type_e::sly3_normal2)
				{
					emit_uniforms((int)draw_type);

					struct mesh_draw_vertex_28
					{
						vec3be pos;
						u32 diff;
						u32 spec;
						vec2be uv;
					};
					static_assert(sizeof(mesh_draw_vertex_28) == 28);
					if (stride != 28) __debugbreak();

					const mesh_draw_vertex_28* vertex_data = (mesh_draw_vertex_28*)block0.vertex_data.data();
					vertex_count                           = block0.vertex_data.size() / sizeof(mesh_draw_vertex_28);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v  = vertex_data[i];
						const vec3 pos = transform_pos(i, v.pos, block1_weights);

#if MESHDUMP_NOCLIP
						obj_str += fmt::format("v36s %f %f %f %f %f %X %X\n",
							pos.x, pos.y, pos.z, (float)v.uv.u, (float)v.uv.v, v.diff, v.spec);
#else
						obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
						obj_str += fmt::format("vt %f %f\n", (float)v.uv.u, (float)v.uv.v);
#endif
					}

					vertex_index_base_normal_offset += vertex_count;
					has_normals = false;
				}
				else if (draw_type == draw_type_e::sly3_skydome2)
				{
					emit_uniforms((int)draw_type);

					struct mesh_draw_vertex_28
					{
						vec3be pos;
						u32 unk[2];
						vec2be uv;
					};
					static_assert(sizeof(mesh_draw_vertex_28) == 28);
					if (stride != 28)
						__debugbreak(); // obj_str += fmt::format("# errordbg %d\n", stride);

					const mesh_draw_vertex_28* vertex_data = (mesh_draw_vertex_28*)block0.vertex_data.data();
					vertex_count                           = block0.vertex_data.size() / sizeof(mesh_draw_vertex_28);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v  = vertex_data[i];
						const vec3 pos = transform_pos(i, v.pos, block1_weights);

						obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
						obj_str += fmt::format("vt %f %f\n", (float)v.uv.u, (float)v.uv.v);
					}

					vertex_index_base_normal_offset += vertex_count;
					has_normals = false;
				}
				else if (draw_type == draw_type_e::sly3_particle)
				{
					obj_str += fmt::format("# skipping sly3_particle\n");
				}
				else if (draw_type == draw_type_e::sly3_particle2)
				{
					obj_str += fmt::format("# skipping sly3_particle2\n");
				}
				else
				{
					obj_str += fmt::format("# unrecognized dump %s\n", dump_name);
#if MESHDUMP_DEBUG
					//__debugbreak();
#endif
				}
			}
			else
			{
				obj_str += fmt::format("# unrecognized non interleaved dump %s\n", dump_name);
			}
		}

#if MESHDUMP_DEBUG
		if (d.vertex_constants_buffer.size() >= 4)
		{
			obj_str += fmt::format("# xform matrix at 0:\n%s\n",
				print_mat4(d.vertex_constants_buffer[0], d.vertex_constants_buffer[1], d.vertex_constants_buffer[2], d.vertex_constants_buffer[3]));
		}
		if (d.vertex_constants_buffer.size() >= 17)
		{
			obj_str += fmt::format("# xform matrix at 13:\n%s\n",
				print_mat4(d.vertex_constants_buffer[13], d.vertex_constants_buffer[14], d.vertex_constants_buffer[15], d.vertex_constants_buffer[16]));
		}
		if (d.vertex_constants_buffer.size() >= 44)
		{
			obj_str += fmt::format("# xform matrices at 32:\n%s\n%s\n%s\n%s\n",
				print_mat43(d.vertex_constants_buffer[32], d.vertex_constants_buffer[33], d.vertex_constants_buffer[34]),
				print_mat43(d.vertex_constants_buffer[35], d.vertex_constants_buffer[36], d.vertex_constants_buffer[37]),
				print_mat43(d.vertex_constants_buffer[38], d.vertex_constants_buffer[39], d.vertex_constants_buffer[40]),
				print_mat43(d.vertex_constants_buffer[41], d.vertex_constants_buffer[42], d.vertex_constants_buffer[43]));
		}

		// and vc[18].xy is tex anim

#endif
#endif

#if MESHDUMP_DEBUG
#if MESHDUMP_GAME <= SLY_4
		obj_str += fmt::format("# draw_type: %d\n", (int)draw_type);
#endif
		obj_str += fmt::format("# tex_count: %d\n", tex_count);
		obj_str += fmt::format("# transform_branch_bits: 0x%X\n", d.transform_branch_bits);

		obj_str += "# VertexConstantsBuffer:\n";
		for (auto i = 0; i < 60; i++)
			obj_str += fmt::format(" # %02d: %s\n", i, print_vec4(d.vertex_constants_buffer[i]));
#endif

#if MESHDUMP_GAME == SLY_4

#if MESHDUMP_DEBUG
		if (d.vertex_constants_buffer.size() >= 4)
			obj_str += fmt::format("# xform matrix at 0:\n%s\n",
				print_mat4(d.vertex_constants_buffer[0], d.vertex_constants_buffer[1], d.vertex_constants_buffer[2], d.vertex_constants_buffer[3]));
#endif

		if (block_count > 0)
		{
			const auto& block0 = d.blocks[0];

			if (block0.interleaved_range_info.interleaved)
			{

				mat4 xform_mat = linalg::identity;

				if (vcb.size() >= 4)
				{
					xform_mat = {vcb[0], vcb[1], vcb[2], vcb[3]};
				}

				auto transform_pos = [&](u32 idx, const auto& pos /*, const mesh_draw_dump_block* weights_block*/) -> vec3 {
					vec4 out;
					vec4 a = {pos.x, pos.y, pos.z, 1};

					static volatile bool s_posed = true;

					if (s_posed)
						out = mul(a, xform_mat);
					else
						out = a;

					return {-out.x, out.y, out.z};
				};

				if (block0.interleaved_range_info.attribute_stride == 24 && block_count == 2)
				{
					const auto& block1 = d.blocks[1];
					struct mesh_draw_vertex_24
					{
						vec3be pos;
						u8 unk[12];
					};
					static_assert(sizeof(mesh_draw_vertex_24) == 24);

					struct mesh_draw_vertex_24_block1
					{
						u32 unk_color;
						u16 texcoord_u;
						u16 texcoord_v;
					};
					static_assert(sizeof(mesh_draw_vertex_24_block1) == 8);

					const mesh_draw_vertex_24* vertex_data_0        = (mesh_draw_vertex_24*)block0.vertex_data.data();
					const mesh_draw_vertex_24_block1* vertex_data_1 = (mesh_draw_vertex_24_block1*)block1.vertex_data.data();
					vertex_count                                    = block0.vertex_data.size() / sizeof(mesh_draw_vertex_24);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v        = vertex_data_0[i];
						const auto& v_block1 = vertex_data_1[i];

						vec3 pos = transform_pos(i, v.pos);
						//obj_str += fmt::format("v %f %f %f\n", (float)v.pos.x * .01, (float)v.pos.z * .01, (float)v.pos.y * .01);
						obj_str += fmt::format("v %f %f %f\n", pos.x, pos.y, pos.z);
						//obj_str += fmt::format("vn %f %f %f\n", (float)v.normal.x, (float)v.normal.y, (float)v.normal.z);
						half texcoord_u;
						texcoord_u.bits = _byteswap_ushort(v_block1.texcoord_u);
						half texcoord_v;
						texcoord_v.bits = _byteswap_ushort(v_block1.texcoord_v);
						obj_str += fmt::format("vt %f %f\n", (float)texcoord_u, (float)texcoord_v);
					}
					vertex_format = vertex_format_t::sly4_24;
				}
				else if (block0.interleaved_range_info.attribute_stride == 14 && block_count == 1)
				{
					struct mesh_draw_vertex_14
					{
						u16 pos_x;
						u16 pos_y;
						u16 pos_z;
						u8 color[4]; // weird format
						u16 texcoord_u;
						u16 texcoord_v;
					};
					static_assert(sizeof(mesh_draw_vertex_14) == 14);

					const mesh_draw_vertex_14* vertex_data_0 = (mesh_draw_vertex_14*)block0.vertex_data.data();
					vertex_count                             = block0.vertex_data.size() / sizeof(mesh_draw_vertex_14);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v = vertex_data_0[i];

						half pos_x;
						pos_x.bits = _byteswap_ushort(v.pos_x);
						half pos_y;
						pos_y.bits = _byteswap_ushort(v.pos_y);
						half pos_z;
						pos_z.bits = _byteswap_ushort(v.pos_z);
						vec3 pos   = vec3{(float)pos_x, (float)pos_y, (float)pos_z};
						pos        = transform_pos(i, pos);
						//obj_str += fmt::format("v %f %f %f\n", (float)v.pos.x * .01, (float)v.pos.z * .01, (float)v.pos.y * .01);
						obj_str += fmt::format("v %f %f %f\n", (float)pos.x, (float)pos.y, (float)pos.z);
						//obj_str += fmt::format("vn %f %f %f\n", (float)v.normal.x, (float)v.normal.y, (float)v.normal.z);
						half texcoord_u;
						texcoord_u.bits = _byteswap_ushort(v.texcoord_u);
						half texcoord_v;
						texcoord_v.bits = _byteswap_ushort(v.texcoord_v);
						obj_str += fmt::format("vt %f %f\n", (float)texcoord_u, (float)texcoord_v);
					}
					vertex_format = vertex_format_t::sly4_14;
				}
				else if (block0.interleaved_range_info.attribute_stride == 18 && block_count == 1)
				{
					struct mesh_draw_vertex_18
					{
						u16 pos_x;
						u16 pos_y;
						u16 pos_z;
						u8 unk0[4]; // weird format
						u16 texcoord_u;
						u16 texcoord_v;
						u8 unk1[4]; // weird format
					};
					static_assert(sizeof(mesh_draw_vertex_18) == 18);

					const mesh_draw_vertex_18* vertex_data_0 = (mesh_draw_vertex_18*)block0.vertex_data.data();
					vertex_count                             = block0.vertex_data.size() / sizeof(mesh_draw_vertex_18);

					for (auto i = 0; i < vertex_count; ++i)
					{
						const auto& v = vertex_data_0[i];

						half pos_x;
						pos_x.bits = _byteswap_ushort(v.pos_x);
						half pos_y;
						pos_y.bits = _byteswap_ushort(v.pos_y);
						half pos_z;
						pos_z.bits = _byteswap_ushort(v.pos_z);
						vec3 pos   = vec3{(float)pos_x, (float)pos_y, (float)pos_z};
						pos        = transform_pos(i, pos);
						//obj_str += fmt::format("v %f %f %f\n", (float)v.pos.x * .01, (float)v.pos.z * .01, (float)v.pos.y * .01);
						obj_str += fmt::format("v %f %f %f\n", (float)pos.x, (float)pos.y, (float)pos.z);
						//obj_str += fmt::format("vn %f %f %f\n", (float)v.normal.x, (float)v.normal.y, (float)v.normal.z);
						half texcoord_u;
						texcoord_u.bits = _byteswap_ushort(v.texcoord_u);
						half texcoord_v;
						texcoord_v.bits = _byteswap_ushort(v.texcoord_v);
						obj_str += fmt::format("vt %f %f\n", (float)texcoord_u, (float)texcoord_v);
					}
					vertex_format = vertex_format_t::sly4_18;
				}
			}
		}
#endif

		if (vertex_count > 0 && index_count > 0)
		{
#if MESHDUMP_NOCLIP && MESHDUMP_NOCLIP_BATCH_DUMPS
			if (new_dump)
				vertex_index_base = 0;
#endif

#if MESHDUMP_DEBUG
			obj_str += fmt::format("# base is %d\n", vertex_index_base);
#endif

			u32 min_idx = *std::min_element(d.indices.begin(), d.indices.end());
#if MESHDUMP_DEBUG
			if (min_idx > 0)
				obj_str += fmt::format("# warning: min_idx is %d\n", min_idx);
#endif

#if MESHDUMP_NOCLIP
			for (auto tri_idx = 0; tri_idx < d.indices.size() / 3; tri_idx++)
			{
				const auto f0 = vertex_index_base + d.indices[tri_idx * 3 + 0] - min_idx;
				const auto f1 = vertex_index_base + d.indices[tri_idx * 3 + 2] - min_idx;
				const auto f2 = vertex_index_base + d.indices[tri_idx * 3 + 1] - min_idx;
				obj_str += fmt::format("f %d %d %d\n", f0, f1, f2);
			}
#else
			if (has_normals)
			{
				for (auto tri_idx = 0; tri_idx < d.indices.size() / 3; tri_idx++)
				{
					const auto f0 = vertex_index_base + d.indices[tri_idx * 3 + 0] - min_idx;
					const auto f1 = vertex_index_base + d.indices[tri_idx * 3 + 2] - min_idx;
					const auto f2 = vertex_index_base + d.indices[tri_idx * 3 + 1] - min_idx;
					obj_str += fmt::format("f %d/%d/%d %d/%d/%d %d/%d/%d\n",
						f0, f0, f0 - vertex_index_base_normal_offset,
						f1, f1, f1 - vertex_index_base_normal_offset,
						f2, f2, f2 - vertex_index_base_normal_offset);
				}
			}
			else
			{
				for (auto tri_idx = 0; tri_idx < d.indices.size() / 3; tri_idx++)
				{
					const auto f0 = vertex_index_base + d.indices[tri_idx * 3 + 0] - min_idx;
					const auto f1 = vertex_index_base + d.indices[tri_idx * 3 + 2] - min_idx;
					const auto f2 = vertex_index_base + d.indices[tri_idx * 3 + 1] - min_idx;
					obj_str += fmt::format("f %d/%d %d/%d %d/%d\n",
						f0, f0,
						f1, f1,
						f2, f2);
				}
			}
#endif

#if !MESHDUMP_NOCLIP || MESHDUMP_NOCLIP_BATCH_DUMPS
			vertex_index_base += vertex_count;
#endif
		}
		else
		{
			obj_str += fmt::format("# warning: unsupported, no vertices will be emitted\n");
		}
	}


	file_obj.write(obj_str.c_str(), obj_str.size());

	dump_index++;

	g_dump_texture_info.clear();
	g_mesh_dumper.enabled = false;
	//g_mesh_dumper.enable_this_frame = false;
	//g_mesh_dumper.enable_this_frame2 = false;
	g_mesh_dumper.dumps.clear();

	Emu.Resume();
}
