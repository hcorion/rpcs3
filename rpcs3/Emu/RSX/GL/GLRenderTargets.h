#pragma once
#include "../Common/surface_store.h"
#include "GLHelpers.h"
#include "../rsx_utils.h"

struct color_swizzle
{
	gl::texture::channel a = gl::texture::channel::a;
	gl::texture::channel r = gl::texture::channel::r;
	gl::texture::channel g = gl::texture::channel::g;
	gl::texture::channel b = gl::texture::channel::b;

	color_swizzle() = default;
	color_swizzle(gl::texture::channel a, gl::texture::channel r, gl::texture::channel g, gl::texture::channel b)
		: a(a), r(r), g(g), b(b)
	{
	}
};

struct color_format
{
	gl::texture::type type;
	gl::texture::format format;
	gl::texture::internal_format internal_format;
	bool swap_bytes;
	color_swizzle swizzle;
};

struct depth_format
{
	gl::texture::type type;
	gl::texture::format format;
	gl::texture::internal_format internal_format;
};

namespace rsx
{
	namespace internals
	{
		color_format surface_color_format_to_gl(rsx::surface_color_format color_format);
		depth_format surface_depth_format_to_gl(rsx::surface_depth_format2 depth_format);
		u8 get_pixel_size(rsx::surface_depth_format format);
	}
}

namespace gl
{
	class render_target : public viewable_image, public rsx::ref_counted, public rsx::render_target_descriptor<texture*>
	{
		void clear_memory(gl::command_context& cmd);
		void load_memory(gl::command_context& cmd);
		void initialize_memory(gl::command_context& cmd, rsx::surface_access access);

	public:
		render_target(GLuint width, GLuint height, GLenum sized_format, rsx::format_class format_class)
			: viewable_image(GL_TEXTURE_2D, width, height, 1, 1, sized_format, format_class)
		{}

		// Internal pitch is the actual row length in bytes of the openGL texture
		void set_native_pitch(u32 pitch)
		{
			native_pitch = pitch;
		}

		void set_surface_dimensions(u16 w, u16 h, u32 pitch)
		{
			surface_width = w;
			surface_height = h;
			rsx_pitch = pitch;
		}

		void set_rsx_pitch(u32 pitch)
		{
			rsx_pitch = pitch;
		}

		bool is_depth_surface() const override
		{
			return !!(aspect() & gl::image_aspect::depth);
		}

		void release_ref(texture* t) const override
		{
			static_cast<gl::render_target*>(t)->release();
		}

		viewable_image* get_surface(rsx::surface_access /*access_type*/) override
		{
			// TODO
			return static_cast<gl::viewable_image*>(this);
		}

		u32 raw_handle() const
		{
			return id();
		}

		bool matches_dimensions(u16 _width, u16 _height) const
		{
			//Use forward scaling to account for rounding and clamping errors
			const auto [scaled_w, scaled_h] = rsx::apply_resolution_scale<true>(_width, _height);
			return (scaled_w == width()) && (scaled_h == height());
		}

		void memory_barrier(gl::command_context& cmd, rsx::surface_access access);
		void read_barrier(gl::command_context& cmd) { memory_barrier(cmd, rsx::surface_access::shader_read); }
		void write_barrier(gl::command_context& cmd) { memory_barrier(cmd, rsx::surface_access::shader_write); }
	};

	struct framebuffer_holder : public gl::fbo, public rsx::ref_counted
	{
		using gl::fbo::fbo;
	};

	static inline gl::render_target* as_rtt(gl::texture* t)
	{
		return ensure(dynamic_cast<gl::render_target*>(t));
	}
}

struct gl_render_target_traits
{
	using surface_storage_type = std::unique_ptr<gl::render_target>;
	using surface_type = gl::render_target*;
	using command_list_type = gl::command_context&;
	using download_buffer_object = std::vector<u8>;
	using barrier_descriptor_t = rsx::deferred_clipped_region<gl::render_target*>;

	static
	std::unique_ptr<gl::render_target> create_new_surface(
		u32 address,
		rsx::surface_color_format surface_color_format,
		usz width, usz height, usz pitch,
		rsx::surface_antialiasing antialias
	)
	{
		auto format = rsx::internals::surface_color_format_to_gl(surface_color_format);
		const auto [width_, height_] = rsx::apply_resolution_scale<true>(static_cast<u16>(width), static_cast<u16>(height));

		std::unique_ptr<gl::render_target> result(new gl::render_target(width_, height_,
			static_cast<GLenum>(format.internal_format), RSX_FORMAT_CLASS_COLOR));

		result->set_aa_mode(antialias);
		result->set_native_pitch(static_cast<u32>(width) * get_format_block_size_in_bytes(surface_color_format) * result->samples_x);
		result->set_surface_dimensions(static_cast<u16>(width), static_cast<u16>(height), static_cast<u32>(pitch));
		result->set_format(surface_color_format);

		std::array<GLenum, 4> native_layout = { static_cast<GLenum>(format.swizzle.a), static_cast<GLenum>(format.swizzle.r), static_cast<GLenum>(format.swizzle.g), static_cast<GLenum>(format.swizzle.b) };
		result->set_native_component_layout(native_layout);

		result->memory_usage_flags = rsx::surface_usage_flags::attachment;
		result->state_flags = rsx::surface_state_flags::erase_bkgnd;
		result->queue_tag(address);
		result->add_ref();
		return result;
	}

	static
	std::unique_ptr<gl::render_target> create_new_surface(
			u32 address,
			rsx::surface_depth_format2 surface_depth_format,
			usz width, usz height, usz pitch,
			rsx::surface_antialiasing antialias
		)
	{
		auto format = rsx::internals::surface_depth_format_to_gl(surface_depth_format);
		const auto [width_, height_] = rsx::apply_resolution_scale<true>(static_cast<u16>(width), static_cast<u16>(height));

		std::unique_ptr<gl::render_target> result(new gl::render_target(width_, height_,
			static_cast<GLenum>(format.internal_format), rsx::classify_format(surface_depth_format)));

		result->set_aa_mode(antialias);
		result->set_surface_dimensions(static_cast<u16>(width), static_cast<u16>(height), static_cast<u32>(pitch));
		result->set_format(surface_depth_format);
		result->set_native_pitch(static_cast<u32>(width) * get_format_block_size_in_bytes(surface_depth_format) * result->samples_x);

		std::array<GLenum, 4> native_layout = { GL_RED, GL_RED, GL_RED, GL_RED };
		result->set_native_component_layout(native_layout);

		result->memory_usage_flags = rsx::surface_usage_flags::attachment;
		result->state_flags = rsx::surface_state_flags::erase_bkgnd;
		result->queue_tag(address);
		result->add_ref();
		return result;
	}

	static
	void clone_surface(
		gl::command_context& cmd,
		std::unique_ptr<gl::render_target>& sink, gl::render_target* ref,
		u32 address, barrier_descriptor_t& prev)
	{
		if (!sink)
		{
			auto internal_format = static_cast<GLenum>(ref->get_internal_format());
			const auto [new_w, new_h] = rsx::apply_resolution_scale<true>(prev.width, prev.height,
				ref->get_surface_width<rsx::surface_metrics::pixels>(), ref->get_surface_height<rsx::surface_metrics::pixels>());

			sink = std::make_unique<gl::render_target>(new_w, new_h, internal_format, ref->format_class());
			sink->add_ref();

			sink->memory_usage_flags = rsx::surface_usage_flags::storage;
			sink->state_flags = rsx::surface_state_flags::erase_bkgnd;
			sink->format_info = ref->format_info;

			sink->set_spp(ref->get_spp());
			sink->set_native_pitch(static_cast<u32>(prev.width) * ref->get_bpp() * ref->samples_x);
			sink->set_rsx_pitch(ref->get_rsx_pitch());
			sink->set_surface_dimensions(prev.width, prev.height, ref->get_rsx_pitch());
			sink->set_native_component_layout(ref->get_native_component_layout());
			sink->queue_tag(address);
		}

		prev.target = sink.get();

		if (!sink->old_contents.empty())
		{
			// Deal with this, likely only needs to clear
			if (sink->surface_width > prev.width || sink->surface_height > prev.height)
			{
				sink->write_barrier(cmd);
			}
			else
			{
				sink->clear_rw_barrier();
			}
		}

		sink->set_rsx_pitch(ref->get_rsx_pitch());
		sink->set_old_contents_region(prev, false);
		sink->last_use_tag = ref->last_use_tag;
		sink->raster_type = ref->raster_type;     // Can't actually cut up swizzled data
	}

	static
	bool is_compatible_surface(const gl::render_target* surface, const gl::render_target* ref, u16 width, u16 height, u8 sample_count)
	{
		return (surface->get_internal_format() == ref->get_internal_format() &&
				surface->get_spp() == sample_count &&
				surface->get_surface_width<rsx::surface_metrics::pixels>() >= width &&
				surface->get_surface_height<rsx::surface_metrics::pixels>() >= height);
	}

	static
	void prepare_surface_for_drawing(gl::command_context&, gl::render_target* surface)
	{
		surface->memory_usage_flags |= rsx::surface_usage_flags::attachment;
	}

	static
	void prepare_surface_for_sampling(gl::command_context&, gl::render_target*)
	{}

	static
	bool surface_is_pitch_compatible(const std::unique_ptr<gl::render_target> &surface, usz pitch)
	{
		return surface->get_rsx_pitch() == pitch;
	}

	static
	void invalidate_surface_contents(gl::command_context&, gl::render_target *surface, u32 address, usz pitch)
	{
		surface->set_rsx_pitch(static_cast<u32>(pitch));
		surface->queue_tag(address);
		surface->last_use_tag = 0;
		surface->stencil_init_flags = 0;
		surface->memory_usage_flags = rsx::surface_usage_flags::unknown;
		surface->raster_type = rsx::surface_raster_type::linear;
	}

	static
	void notify_surface_invalidated(const std::unique_ptr<gl::render_target>& surface)
	{
		if (!surface->old_contents.empty())
		{
			// TODO: Retire the deferred writes
			surface->clear_rw_barrier();
		}

		surface->release();
	}

	static
	void notify_surface_persist(const std::unique_ptr<gl::render_target>& /*surface*/)
	{}

	static
	void notify_surface_reused(const std::unique_ptr<gl::render_target>& surface)
	{
		surface->state_flags |= rsx::surface_state_flags::erase_bkgnd;
		surface->add_ref();
	}

	static
	bool int_surface_matches_properties(
		const std::unique_ptr<gl::render_target> &surface,
		gl::texture::internal_format format,
		usz width, usz height,
		rsx::surface_antialiasing antialias,
		bool check_refs = false)
	{
		if (check_refs && surface->has_refs())
			return false;

		return surface->get_internal_format() == format &&
			surface->get_spp() == get_format_sample_count(antialias) &&
			surface->matches_dimensions(static_cast<u16>(width), static_cast<u16>(height));
	}

	static
	bool surface_matches_properties(
		const std::unique_ptr<gl::render_target> &surface,
		rsx::surface_color_format format,
		usz width, usz height,
		rsx::surface_antialiasing antialias,
		bool check_refs=false)
	{
		const auto internal_fmt = rsx::internals::surface_color_format_to_gl(format).internal_format;
		return int_surface_matches_properties(surface, internal_fmt, width, height, antialias, check_refs);
	}

	static
	bool surface_matches_properties(
		const std::unique_ptr<gl::render_target> &surface,
		rsx::surface_depth_format2 format,
		usz width, usz height,
		rsx::surface_antialiasing antialias,
		bool check_refs = false)
	{
		const auto internal_fmt = rsx::internals::surface_depth_format_to_gl(format).internal_format;
		return int_surface_matches_properties(surface, internal_fmt, width, height, antialias, check_refs);
	}

	static
	gl::render_target* get(const std::unique_ptr<gl::render_target> &in)
	{
		return in.get();
	}
};

struct gl_render_targets : public rsx::surface_store<gl_render_target_traits>
{
	void destroy()
	{
		invalidate_all();
		invalidated_resources.clear();
	}

	std::vector<GLuint> free_invalidated(gl::command_context& cmd)
	{
		// Do not allow more than 256M of RSX memory to be used by RTTs
		if (check_memory_usage(256 * 0x100000))
		{
			handle_memory_pressure(cmd, rsx::problem_severity::moderate);
		}

		std::vector<GLuint> removed;
		invalidated_resources.remove_if([&](auto &rtt)
		{
			if (rtt->unused_check_count() >= 2)
			{
				removed.push_back(rtt->id());
				return true;
			}

			return false;
		});

		return removed;
	}
};
