#pragma once

#include "stdafx.h"
#include "GLHelpers.h"
#include "../overlays.h"

extern u64 get_system_time();

namespace gl
{
	struct overlay_pass
	{
		std::string fs_src;
		std::string vs_src;

		gl::glsl::program program_handle;
		gl::glsl::shader vs;
		gl::glsl::shader fs;

		gl::fbo fbo;

		gl::vao m_vao;
		gl::buffer m_vertex_data_buffer;

		bool compiled = false;

		u32 num_drawable_elements = 4;
		GLenum primitives = GL_TRIANGLE_STRIP;

		void create()
		{
			if (!compiled)
			{
				fs.create(gl::glsl::shader::type::fragment);
				fs.source(fs_src);
				fs.compile();

				vs.create(gl::glsl::shader::type::vertex);
				vs.source(vs_src);
				vs.compile();

				program_handle.create();
				program_handle.attach(vs);
				program_handle.attach(fs);
				program_handle.make();

				fbo.create();

				m_vertex_data_buffer.create();

				int old_vao;
				glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);

				m_vao.create();
				m_vao.bind();

				m_vao.array_buffer = m_vertex_data_buffer;
				m_vao[0] = buffer_pointer(&m_vao);

				glBindVertexArray(old_vao);

				compiled = true;
			}
		}

		void destroy()
		{
			if (compiled)
			{
				program_handle.remove();
				vs.remove();
				fs.remove();

				fbo.remove();
				m_vao.remove();
				m_vertex_data_buffer.remove();

				compiled = false;
			}
		}

		virtual void on_load() {}
		virtual void on_unload() {}

		virtual void bind_resources() {}
		virtual void cleanup_resources() {}

		virtual void upload_vertex_data(f32* data, u32 elements_count)
		{
			elements_count <<= 2;
			m_vertex_data_buffer.data(elements_count, data);
		}

		virtual void emit_geometry()
		{
			int old_vao;
			glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);

			m_vao.bind();
			glDrawArrays(primitives, 0, num_drawable_elements);

			glBindVertexArray(old_vao);
		}

		virtual void run(u16 w, u16 h, GLuint target_texture, bool depth_target, bool use_blending = false)
		{
			if (!compiled)
			{
				LOG_ERROR(RSX, "You must initialize overlay passes with create() before calling run()");
				return;
			}

			GLint program;
			GLint old_fbo;
			GLint depth_func;
			GLint viewport[4];
			GLboolean color_writes[4];
			GLboolean depth_write;

			GLint blend_src_rgb;
			GLint blend_src_a;
			GLint blend_dst_rgb;
			GLint blend_dst_a;
			GLint blend_eq_a;
			GLint blend_eq_rgb;

			if (target_texture)
			{
				glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
				glBindFramebuffer(GL_FRAMEBUFFER, fbo.id());

				if (depth_target)
				{
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, target_texture, 0);
					glDrawBuffer(GL_NONE);
				}
				else
				{
					GLenum buffer = GL_COLOR_ATTACHMENT0;
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0);
					glDrawBuffers(1, &buffer);
				}
			}

			if (!target_texture || glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
			{
				// Push rasterizer state
				glGetIntegerv(GL_VIEWPORT, viewport);
				glGetBooleanv(GL_COLOR_WRITEMASK, color_writes);
				glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
				glGetIntegerv(GL_CURRENT_PROGRAM, &program);
				glGetIntegerv(GL_DEPTH_FUNC, &depth_func);

				GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
				GLboolean depth_test_enabled = glIsEnabled(GL_DEPTH_TEST);
				GLboolean cull_face_enabled = glIsEnabled(GL_CULL_FACE);
				GLboolean blend_enabled = glIsEnabled(GL_BLEND);
				GLboolean stencil_test_enabled = glIsEnabled(GL_STENCIL_TEST);

				if (use_blending)
				{
					glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
					glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
					glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
					glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
					glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
					glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_a);
				}

				// Set initial state
				glViewport(0, 0, w, h);
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glDepthMask(depth_target ? GL_TRUE : GL_FALSE);

				// Disabling depth test will also disable depth writes which is not desired
				glDepthFunc(GL_ALWAYS);
				glEnable(GL_DEPTH_TEST);

				if (scissor_enabled) glDisable(GL_SCISSOR_TEST);
				if (cull_face_enabled) glDisable(GL_CULL_FACE);
				if (stencil_test_enabled) glDisable(GL_STENCIL_TEST);

				if (use_blending)
				{
					if (!blend_enabled)
						glEnable(GL_BLEND);

					glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				}
				else if (blend_enabled)
				{
					glDisable(GL_BLEND);
				}

				// Render
				program_handle.use();
				on_load();
				bind_resources();
				emit_geometry();

				// Clean up
				if (target_texture)
				{
					if (depth_target)
						glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
					else
						glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

					glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
				}

				glUseProgram((GLuint)program);

				glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
				glColorMask(color_writes[0], color_writes[1], color_writes[2], color_writes[3]);
				glDepthMask(depth_write);
				glDepthFunc(depth_func);

				if (!depth_test_enabled) glDisable(GL_DEPTH_TEST);
				if (scissor_enabled) glEnable(GL_SCISSOR_TEST);
				if (cull_face_enabled) glEnable(GL_CULL_FACE);
				if (stencil_test_enabled) glEnable(GL_STENCIL_TEST);

				if (use_blending)
				{
					if (!blend_enabled)
						glDisable(GL_BLEND);

					glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
					glBlendEquationSeparate(blend_eq_rgb, blend_eq_a);
				}
				else if (blend_enabled)
				{
					 glEnable(GL_BLEND);
				}
			}
			else
			{
				LOG_ERROR(RSX, "Overlay pass failed because framebuffer was not complete. Run with debug output enabled to diagnose the problem");
			}
		}
	};

	struct depth_convert_pass : public overlay_pass
	{
		depth_convert_pass()
		{
			vs_src =
			{
				"#version 420\n\n"
				"out vec2 tc0;\n"
				"\n"
				"void main()\n"
				"{\n"
				"	vec2 positions[] = {vec2(-1., -1.), vec2(1., -1.), vec2(-1., 1.), vec2(1., 1.)};\n"
				"	vec2 coords[] = {vec2(0., 0.), vec2(1., 0.), vec2(0., 1.), vec2(1., 1.)};\n"
				"	gl_Position = vec4(positions[gl_VertexID % 4], 0., 1.);\n"
				"	tc0 = coords[gl_VertexID % 4];\n"
				"}\n"
			};

			fs_src =
			{
				"#version 420\n\n"
				"in vec2 tc0;\n"
				"layout(binding=31) uniform sampler2D fs0;\n"
				"\n"
				"void main()\n"
				"{\n"
				"	vec4 rgba_in = texture(fs0, tc0);\n"
				"	gl_FragDepth = rgba_in.w * 0.99609 + rgba_in.x * 0.00389 + rgba_in.y * 0.00002;\n"
				"}\n"
			};
		}

		void run(u16 w, u16 h, GLuint target, GLuint source)
		{
			glActiveTexture(GL_TEXTURE31);
			glBindTexture(GL_TEXTURE_2D, source);

			overlay_pass::run(w, h, target, true);
		}
	};

	struct rgba8_unorm_rg16_sfloat_convert_pass : public overlay_pass
	{
		//Not really needed since directly copying data via ARB_copy_image works out fine
		rgba8_unorm_rg16_sfloat_convert_pass()
		{
			vs_src =
			{
				"#version 420\n\n"
				"\n"
				"void main()\n"
				"{\n"
				"	vec2 positions[] = {vec2(-1., -1.), vec2(1., -1.), vec2(-1., 1.), vec2(1., 1.)};\n"
				"	gl_Position = vec4(positions[gl_VertexID % 4], 0., 1.);\n"
				"}\n"
			};

			fs_src =
			{
				"#version 420\n\n"
				"layout(binding=31) uniform sampler2D fs0;\n"
				"layout(location=0) out vec4 ocol;\n"
				"\n"
				"void main()\n"
				"{\n"
				"	uint value = packUnorm4x8(texelFetch(fs0, ivec2(gl_FragCoord.xy), 0).zyxw);\n"
				"	ocol.xy = unpackHalf2x16(value);\n"
				"}\n"
			};
		}

		void run(u16 w, u16 h, GLuint target, GLuint source)
		{
			glActiveTexture(GL_TEXTURE31);
			glBindTexture(GL_TEXTURE_2D, source);

			overlay_pass::run(w, h, target, false);
		}
	};

	struct ui_overlay_text : public overlay_pass
	{
		ui_overlay_text()
		{
			vs_src =
			{
				"#version 420\n\n"
				"layout(location=0) in vec4 in_pos;\n"
				"layout(location=0) out vec2 tc0;\n"
				"uniform vec4 ui_scale_parameters;\n"
				"\n"
				"void main()\n"
				"{\n"
				"	const vec2 offsets[] = {vec2(0., 0.), vec2(1., 0.), vec2(1., 1.), vec2(0., 1.)};\n"
				"	vec2 pos = offsets[gl_VertexID % 4] * ui_scale_parameters.xy;\n"
				"	tc0 = offsets[gl_VertexID % 4] * ui_scale_parameters.zw;\n"
				"	tc0.y += (in_pos.z / 16.) / 16.;\n"
				"	tc0.x += (in_pos.z % 16.) / 16.;\n"
				"	gl_Position = vec4(pos, 0., 1.);\n"
				"}\n"
			};

			fs_src =
			{
				"#version 420\n\n"
				"layout(binding=31) uniform sampler2D fs0;\n"
				"layout(location=0) in vec2 tc0;\n"
				"layout(location=0) out vec4 ocol;\n"
				"\n"
				"void main()\n"
				"{\n"
				"	ocol = texture(fs0, tc0).xxxx;\n"
				"}\n"
			};
		}

		void load_config()
		{

		}

		void run(u16 w, u16 h, GLuint target, GLuint source, rsx::overlays::user_interface& ui)
		{
			glActiveTexture(GL_TEXTURE31);
			glBindTexture(GL_TEXTURE_2D, source);

			//Set up vaos, etc
			overlay_pass::run(w, h, target, false);
		}
	};

	struct ui_overlay_renderer : public overlay_pass
	{
		u32 num_elements = 0;
		std::vector<std::unique_ptr<gl::texture>> resources;
		std::unordered_map<u64, std::unique_ptr<gl::texture>> font_cache;
		bool is_font_draw = false;

		ui_overlay_renderer()
		{
			vs_src =
			{
				"#version 420\n\n"
				"layout(location=0) in vec4 in_pos;\n"
				"layout(location=0) out vec2 tc0;\n"
				"uniform vec4 ui_scale;\n"
				"\n"
				"void main()\n"
				"{\n"
				"	tc0.xy = in_pos.zw;\n"
				"	vec4 pos = vec4((in_pos.xy * ui_scale.zw) / ui_scale.xy, 0., 1.);\n"
				"	pos.y = (1. - pos.y);\n"
				"	gl_Position = (pos + pos) - 1.;\n"
				"}\n"
			};

			fs_src =
			{
				"#version 420\n\n"
				"layout(binding=31) uniform sampler2D fs0;\n"
				"layout(location=0) in vec2 tc0;\n"
				"layout(location=0) out vec4 ocol;\n"
				"uniform vec4 color;\n"
				"uniform float time;\n"
				"uniform int read_texture;\n"
				"uniform int pulse_glow;\n"
				"\n"
				"void main()\n"
				"{\n"
				"	vec4 diff_color = color;\n"
				"	if (pulse_glow != 0)\n"
				"		diff_color.a *= (sin(time) + 1.f) * 0.5f;\n"
				"\n"
				"	if (read_texture != 0)\n"
				"		ocol = texture(fs0, tc0) * diff_color;\n"
				"	else\n"
				"		ocol = diff_color;\n"
				"}\n"
			};
		}

		void create()
		{
			overlay_pass::create();

			rsx::overlays::resource_config configuration;
			configuration.load_files();

			for (const auto &res : configuration.texture_raw_data)
			{
				auto tex = std::make_unique<gl::texture>(gl::texture::target::texture2D);
				tex->create();
				tex->config()
					.size({ res->w, res->h })
					.format(gl::texture::format::rgba)
					.type(gl::texture::type::uint_8_8_8_8)
					.wrap(gl::texture::wrap::clamp_to_border, gl::texture::wrap::clamp_to_border, gl::texture::wrap::clamp_to_border)
					.swizzle(gl::texture::channel::a, gl::texture::channel::b, gl::texture::channel::g, gl::texture::channel::r)
					.apply();
				tex->copy_from(res->data, gl::texture::format::rgba, gl::texture::type::uint_8_8_8_8);
				resources.push_back(std::move(tex));
			}

			configuration.free_resources();
		}

		void destroy()
		{
			resources.clear();
			font_cache.clear();
			overlay_pass::destroy();
		}

		gl::texture* find_font(rsx::overlays::font *font)
		{
			u64 key = (u64)font;
			auto found = font_cache.find(key);
			if (found != font_cache.end())
				return found->second.get();

			//Create font file
			auto tex = std::make_unique<gl::texture>(gl::texture::target::texture2D);
			tex->create();
			tex->config()
				.size({ (int)font->width, (int)font->height })
				.format(gl::texture::format::r)
				.type(gl::texture::type::ubyte)
				.internal_format(gl::texture::internal_format::r8)
				.wrap(gl::texture::wrap::clamp_to_border, gl::texture::wrap::clamp_to_border, gl::texture::wrap::clamp_to_border)
				.swizzle(gl::texture::channel::r, gl::texture::channel::r, gl::texture::channel::r, gl::texture::channel::r)
				.apply();
			tex->copy_from(font->glyph_data.data(), gl::texture::format::r, gl::texture::type::ubyte);

			auto result = tex.get();
			font_cache[key] = std::move(tex);

			return result;
		}

		void emit_geometry() override
		{
			if (!is_font_draw)
			{
				overlay_pass::emit_geometry();
			}
			else
			{
				int num_quads = num_drawable_elements / 4;
				std::vector<GLint> firsts;
				std::vector<GLsizei> counts;

				firsts.resize(num_quads);
				counts.resize(num_quads);

				for (int n = 0; n < num_quads; ++n)
				{
					firsts[n] = (n * 4);
					counts[n] = 4;
				}

				int old_vao;
				glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);

				m_vao.bind();
				glMultiDrawArrays(GL_TRIANGLE_STRIP, firsts.data(), counts.data(), num_quads);

				glBindVertexArray(old_vao);
			}
		}

		void run(u16 w, u16 h, GLuint target, rsx::overlays::user_interface& ui)
		{
			program_handle.uniforms["ui_scale"] = color4f((f32)ui.virtual_width, (f32)ui.virtual_height, 1.f, 1.f);
			program_handle.uniforms["time"] = (f32)(get_system_time() / 1000) * 0.005f;
			for (auto &cmd : ui.get_compiled().draw_commands)
			{
				upload_vertex_data((f32*)cmd.second.data(), cmd.second.size() * 4);
				num_drawable_elements = cmd.second.size();
				is_font_draw = false;
				GLint texture_exists = GL_TRUE;

				glActiveTexture(GL_TEXTURE31);
				switch (cmd.first.texture_ref)
				{
				case rsx::overlays::image_resource_id::game_icon:
				case rsx::overlays::image_resource_id::backbuffer:
					//TODO
				case rsx::overlays::image_resource_id::none:
					texture_exists = GL_FALSE;
					glBindTexture(GL_TEXTURE_2D, GL_NONE);
					break;
				case rsx::overlays::image_resource_id::font_file:
					is_font_draw = true;
					glBindTexture(GL_TEXTURE_2D, find_font(cmd.first.font_ref)->id());
					break;
				default:
					glBindTexture(GL_TEXTURE_2D, resources[cmd.first.texture_ref - 1]->id());
					break;
				}

				program_handle.uniforms["color"] = cmd.first.color;
				program_handle.uniforms["read_texture"] = texture_exists;
				program_handle.uniforms["pulse_glow"] = (s32)cmd.first.pulse_glow;
				overlay_pass::run(w, h, target, false, true);
			}
		}
	};
}