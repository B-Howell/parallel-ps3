#pragma once

#include "Emu/CPU/turn_calls.h"
#include <map>
#include <string>

// Where each game's disc is, by title ID: what the boot looks up to mount
// /dev_bdvd/ again for a disc game's update or patch, a save state of it or
// an executable it launches from the disc. Kept in memory, for the session:
// the frontend has the game library and hands the core its content, and the
// disc booted registers itself here. RPCS3 kept it in games.yml, read and
// written on every boot, for its own game list.
class games_config
{
public:
	games_config() = default;

	const std::map<std::string, std::string> get_games() const;

	std::string get_path(const std::string& title_id) const;

	enum class result
	{
		failure,
		success,
		exists
	};
	result add_game(const std::string& key, const std::string& path);
	result add_external_hdd_game(const std::string& key, std::string& path);
	result remove_game(const std::string& key);

private:
	std::map<std::string, std::string> m_games;
	// The games change in turn (turn_calls.h): no lock
	mutable turn_calls_cpp m_turns;
};
