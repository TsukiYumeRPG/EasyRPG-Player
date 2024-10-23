/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EP_GAME_MULTIPLAYER_H
#define EP_GAME_MULTIPLAYER_H

#include <string>
#include "../game_config.h"
#include "../game_pictures.h"
#include <lcf/rpg/sound.h>

namespace Multiplayer {

enum DebugTextMode {
	DT_NONE = 0,
	DT_DEFAULT = 1,
	DT_PLAYER_A = 2,
	DT_PLAYER_B = 4,
	DT_PLAYER_FULL = DT_PLAYER_A | DT_PLAYER_B,
};

class Game_Multiplayer {
public:
	static Game_Multiplayer& Instance();

	Game_Multiplayer();

	/**
	 * Client
	 */

	enum class NametagMode {
		NONE,
		CLASSIC,
		COMPACT,
		SLIM
	};
	NametagMode GetNametagMode() { return nametag_mode; }
	void SetNametagMode(int mode) {
		nametag_mode = static_cast<NametagMode>(mode);
	}

	std::string GetDebugText(DebugTextMode mode);
	void ToggleDebugTextOverlayMode(DebugTextMode mode);

	// Config
	void GameLoaded();
	void SetRemoteAddress(std::string address);
	void SetConfig(const Game_ConfigMultiplayer& cfg);
	Game_ConfigMultiplayer& GetConfig();

	// Connection
	bool IsActive();
	void Connect();
	void Disconnect();

	// Chat
	void SetChatName(std::string chat_name);
	std::string GetChatName();
	void SendChatMessage(int visibility, std::string message, int crypt_key_hash = 0);

	// Screen
	void ApplyScreenTone(); // tint screen
	void ApplyFlash(int r, int g, int b, int power, int frames); // flash screen
	void ApplyRepeatingFlashes();

	/**
	 * Sync
	 */

	void SwitchRoom(int map_id, bool from_save = false);
	void MapQuit();
	void Quit();
	void MainPlayerMoved(int dir);
	void MainPlayerFacingChanged(int dir);
	void MainPlayerChangedMoveSpeed(int spd);
	void MainPlayerChangedSpriteGraphic(std::string name, int index);
	void MainPlayerJumped(int x, int y);
	void MainPlayerFlashed(int r, int g, int b, int p, int f);
	void MainPlayerChangedTransparency(int transparency);
	void MainPlayerChangedSpriteHidden(bool hidden);
	void MainPlayerTeleported(int map_id, int x, int y);
	void MainPlayerTriggeredEvent(int event_id, bool action);
	void SystemGraphicChanged(StringView sys);
	void SePlayed(const lcf::rpg::Sound& sound);
	void SwitchSet(int switch_id, int value);
	void VariableSet(int var_id, int value);

	// Picture
	void PictureShown(int pic_id, Game_Pictures::ShowParams& params);
	void PictureMoved(int pic_id, Game_Pictures::MoveParams& params);
	void PictureErased(int pic_id);

	// Battle
	bool IsBattleAnimSynced(int anim_id);
	void PlayerBattleAnimShown(int anim_id);
	void ApplyPlayerBattleAnimUpdates();

	// Virtual3D
	void EventLocationChanged(int event_id, int x, int y);
	int GetTerrainTag(int original_terrain_id, int x, int y);

	/**
	 * Steps
	 */

	void Update();
	void MapUpdate();

private:
	NametagMode nametag_mode{NametagMode::CLASSIC};
};

} // end of namespace

using Game_Multiplayer = Multiplayer::Game_Multiplayer;

inline Game_Multiplayer& GMI() { return Game_Multiplayer::Instance(); }

#endif
