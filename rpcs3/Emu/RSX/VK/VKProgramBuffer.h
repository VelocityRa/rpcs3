#pragma once
#include "VKVertexProgram.h"
#include "VKFragmentProgram.h"
#include "VKPipelineCompiler.h"
#include "../Program/ProgramStateCache.h"

namespace vk
{
	struct VKTraits
	{
		using vertex_program_type = VKVertexProgram;
		using fragment_program_type = VKFragmentProgram;
		using pipeline_type = vk::glsl::program;
		using pipeline_storage_type = std::unique_ptr<vk::glsl::program>;
		using pipeline_properties = vk::pipeline_props;

		static
			void recompile_fragment_program(const RSXFragmentProgram& RSXFP, fragment_program_type& fragmentProgramData, usz ID)
		{
			fragmentProgramData.Decompile(RSXFP);
#if 0
			// GOW UI skip

			#if 0
			const auto test = R"(float in_w = (1. / gl_FragCoord.w);
	h0 = TEX2D(0, vec4(tc0.xy, 0., in_w).xy);
	//FENCT
	h0 = clamp16((h0 * tc1));
	h0.xyz = clamp16((h0 * h0.wwww)).xyz;
	h0.w = h0.w;)";
			#else
			// const auto test = "//FENCT\n\th0 = clamp16((h0 * tc1));\n\th0.xyz = clamp16((h0 * h0.wwww)).xyz;\n\th0.w = clamp16(h0).w;";
			 const auto test = "//FENCT\n\th0 = clamp16((h0 * tc1));\n\th0.xyz = clamp16((h0 * h0.wwww)).xyz;\n\th0.w = h0.w;";
			//const auto test = "h0.xyz = clamp16((h0 * h0.wwww)).xyz;";
			//const auto test = "vec4 param = texture(tex0, (vec4(tc0.xy, 0.0, in_w).xy + vec2(_267.texture_parameters[0].scale_bias.w)) * _267.texture_parameters[0].scale_bias.xy);";
			#endif
			const auto& src = fragmentProgramData.shader.get_source();
			if (src.find(test) != std::string::npos)
			{
				//__debugbreak();
				//fs::file patched_shader(R"(D:\Nikos\Reversing\Sly\Renderdoc\sly2_main.vk.frag.glsl)");
				//const std::string patched_shader_str = patched_shader.to_string();
				std::string patched_shader_str = src;
				//patched_shader_str.insert(src.find(test), "h0 = f16vec4(0.); return;");
				patched_shader_str.insert(src.find(test), "h0 = vec4(0.); return;");
				fragmentProgramData.shader.create(::glsl::program_domain::glsl_fragment_program, patched_shader_str);
			}
#endif

			fragmentProgramData.id = static_cast<u32>(ID);
			fragmentProgramData.Compile();
		}

		static
			void recompile_vertex_program(const RSXVertexProgram& RSXVP, vertex_program_type& vertexProgramData, usz ID)
		{
			vertexProgramData.Decompile(RSXVP);
			vertexProgramData.id = static_cast<u32>(ID);
			vertexProgramData.Compile();
		}

		static
			void validate_pipeline_properties(const VKVertexProgram&, const VKFragmentProgram& fp, vk::pipeline_props& properties)
		{
			//Explicitly disable writing to undefined registers
			properties.state.att_state[0].colorWriteMask &= fp.output_color_masks[0];
			properties.state.att_state[1].colorWriteMask &= fp.output_color_masks[1];
			properties.state.att_state[2].colorWriteMask &= fp.output_color_masks[2];
			properties.state.att_state[3].colorWriteMask &= fp.output_color_masks[3];
		}

		static
			pipeline_type* build_pipeline(
				const vertex_program_type& vertexProgramData,
				const fragment_program_type& fragmentProgramData,
				const vk::pipeline_props& pipelineProperties,
				bool compile_async,
				std::function<pipeline_type*(pipeline_storage_type&)> callback)
		{
			vk::pipe_compiler::op_flags compiler_flags = compile_async ? vk::pipe_compiler::COMPILE_DEFERRED : vk::pipe_compiler::COMPILE_INLINE;
			compiler_flags |= vk::pipe_compiler::SEPARATE_SHADER_OBJECTS;

			auto compiler = vk::get_pipe_compiler();
			auto result = compiler->compile(
				pipelineProperties,
				vertexProgramData.handle,
				fragmentProgramData.handle,
				compiler_flags, callback,
				vertexProgramData.uniforms,
				fragmentProgramData.uniforms);

			return callback(result);
		}
	};

	struct program_cache : public program_state_cache<VKTraits>
	{
		program_cache(decompiler_callback_t callback)
		{
			notify_pipeline_compiled = callback;
		}

		u64 get_hash(const vk::pipeline_props& props)
		{
			return rpcs3::hash_struct<vk::pipeline_props>(props);
		}

		u64 get_hash(const RSXVertexProgram& prog)
		{
			return program_hash_util::vertex_program_utils::get_vertex_program_ucode_hash(prog);
		}

		u64 get_hash(const RSXFragmentProgram& prog)
		{
			return program_hash_util::fragment_program_utils::get_fragment_program_ucode_hash(prog);
		}

		template <typename... Args>
		void add_pipeline_entry(RSXVertexProgram& vp, RSXFragmentProgram& fp, vk::pipeline_props& props, Args&& ...args)
		{
			get_graphics_pipeline(nullptr, vp, fp, props, false, false, std::forward<Args>(args)...);
		}

		void preload_programs(rsx::program_cache_hint_t* cache_hint, const RSXVertexProgram& vp, const RSXFragmentProgram& fp)
		{
			search_vertex_program(cache_hint, vp);
			search_fragment_program(cache_hint, fp);
		}

		bool check_cache_missed() const
		{
			return m_cache_miss_flag;
		}
	};
}
