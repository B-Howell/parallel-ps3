#pragma once

#include "vk_sampler_table.h"
#include "device.h"
#include "shared.h"
#include <functional>

namespace vk
{
	struct border_color_t
	{
		u64 storage_key;
		VkBorderColor value;
		VkFormat format;
		VkImageCreateFlags aspect;
		color4f color_value;

		border_color_t(const color4f& color, VkFormat fmt = VK_FORMAT_UNDEFINED, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT);

		border_color_t(VkBorderColor color);

		bool operator == (const border_color_t& that) const
		{
			if (this->value != that.value)
			{
				return false;
			}

			switch (this->value)
			{
			case VK_BORDER_COLOR_FLOAT_CUSTOM_EXT:
			case VK_BORDER_COLOR_INT_CUSTOM_EXT:
				return this->color_value == that.color_value;
			default:
				return true;
			}
		}
	};

	struct sampler
	{
		VkSampler value;
		VkSamplerCreateInfo info = {};

		sampler(const vk::render_device& dev, VkSamplerAddressMode clamp_u, VkSamplerAddressMode clamp_v, VkSamplerAddressMode clamp_w,
			VkBool32 unnormalized_coordinates, float mipLodBias, float max_anisotropy, float min_lod, float max_lod,
			VkFilter min_filter, VkFilter mag_filter, VkSamplerMipmapMode mipmap_mode, const border_color_t& border_color,
			VkBool32 depth_compare = false, VkCompareOp depth_compare_mode = VK_COMPARE_OP_NEVER);

		~sampler();

		bool matches(VkSamplerAddressMode clamp_u, VkSamplerAddressMode clamp_v, VkSamplerAddressMode clamp_w,
			VkBool32 unnormalized_coordinates, float mipLodBias, float max_anisotropy, float min_lod, float max_lod,
			VkFilter min_filter, VkFilter mag_filter, VkSamplerMipmapMode mipmap_mode, const border_color_t& border_color,
			VkBool32 depth_compare = false, VkCompareOp depth_compare_mode = VK_COMPARE_OP_NEVER);

		sampler(const sampler&) = delete;
		sampler(sampler&&) = delete;
	private:
		VkDevice m_device;
		border_color_t m_border_color;
	};

	// Caching helpers
	struct sampler_pool_key_t
	{
		u64 base_key;
		u64 border_color_key;

		bool operator == (const sampler_pool_key_t& that) const
		{
			return this->base_key == that.base_key &&
				this->border_color_key == that.border_color_key;
		}
	};

	struct cached_sampler_object_t : public vk::sampler, public rsx::ref_counted
	{
		sampler_pool_key_t key;
		using vk::sampler::sampler;
	};

	class sampler_pool_t
	{
		// By key, owned: a fixed table made once (vk_sampler_table.c), where it was two STL hash maps of
		// unique_ptrs, hashed and grown on each new sampler
		std::unique_ptr<vk_sampler_table_t> m_table = std::make_unique<vk_sampler_table_t>();

	public:
		sampler_pool_t() = default;
		sampler_pool_t(const sampler_pool_t&) = delete;
		sampler_pool_t& operator=(const sampler_pool_t&) = delete;
		~sampler_pool_t() { clear(); }

		sampler_pool_key_t compute_storage_key(
			VkSamplerAddressMode clamp_u, VkSamplerAddressMode clamp_v, VkSamplerAddressMode clamp_w,
			VkBool32 unnormalized_coordinates, float mipLodBias, float max_anisotropy, float min_lod, float max_lod,
			VkFilter min_filter, VkFilter mag_filter, VkSamplerMipmapMode mipmap_mode, const vk::border_color_t& border_color,
			VkBool32 depth_compare, VkCompareOp depth_compare_mode);

		void clear();

		cached_sampler_object_t* find(const sampler_pool_key_t& key) const;

		cached_sampler_object_t* emplace(const sampler_pool_key_t& key, std::unique_ptr<cached_sampler_object_t>& object);

		// predicate captures nothing: a plain function (it was a std::function)
		std::vector<std::unique_ptr<cached_sampler_object_t>> collect(bool (*predicate)(const cached_sampler_object_t&));
	};
}
