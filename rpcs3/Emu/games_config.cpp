#include "stdafx.h"
#include "games_config.h"
#include "util/logs.hpp"
#include "Utilities/File.h"

#include "Loader/ISO.h"

LOG_CHANNEL(cfg_log, "CFG");

const std::map<std::string, std::string> games_config::get_games() const
{
	return turn_call(&m_turns, [&]() -> std::map<std::string, std::string>
	{
		return m_games;
	});
}

std::string games_config::get_path(const std::string& title_id) const
{
	if (title_id.empty())
	{
		return {};
	}

	return turn_call(&m_turns, [&]() -> std::string
	{
		if (const auto it = m_games.find(title_id); it != m_games.cend())
		{
			return it->second;
		}

		return {};
	});
}

games_config::result games_config::add_game(const std::string& key, const std::string& path)
{
	if (path == iso_device::virtual_device_name + "/")
	{
		const auto device = fs::get_virtual_device(iso_device::virtual_device_name + "/");
		if (!device) return result::failure;

		const auto iso_dev = dynamic_cast<const iso_device*>(device.get());
		return add_game(key, iso_dev->get_loaded_iso());
	}

	if (fs::is_drive_name(path))
	{
		// Restore the trailing delimiter trimmed by the caller if the game is on the root of a raw device (e.g. "E:/"),
		// which is also the form provided by "rpcs3::utils::get_games_dir()" and the one built by "GetBdvdDir()", so
		// that the same title is always registered with the very same path
		return add_game(key, path + '/');
	}

	return turn_call(&m_turns, [&]() -> result
	{
		// Access or create node if does not exist
		if (auto it = m_games.find(key); it != m_games.end())
		{
			if (it->second == path)
			{
				// Nothing to do
				return result::exists;
			}

			it->second = path;
		}
		else
		{
			m_games.emplace(key, path);
		}

		return result::success;
	});
}

games_config::result games_config::add_external_hdd_game(const std::string& key, std::string& path)
{
	// Don't use the C00 subdirectory in our game list
	if (path.ends_with("/C00") || path.ends_with("\\C00"))
	{
		path = path.substr(0, path.size() - 4);
	}

	const result res = add_game(key, path);

	switch (res)
	{
	case result::failure:
		cfg_log.error("Failed to save HG game location of title '%s' (error=%s)", key, fs::g_tls_error);
		break;
	case result::success:
		cfg_log.notice("Registered HG game directory for title '%s': %s", key, path);
		break;
	case result::exists:
		break;
	}

	return res;
}

games_config::result games_config::remove_game(const std::string& key)
{
	return turn_call(&m_turns, [&]() -> result
	{
		// Remove node
		if (m_games.erase(key) == 0) // If node not found
		{
			// Nothing to do
			return result::success;
		}

		return result::success;
	});
}
