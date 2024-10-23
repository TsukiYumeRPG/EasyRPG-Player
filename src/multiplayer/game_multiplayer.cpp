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

#include <lcf/data.h>
#include <lcf/reader_util.h>

#ifdef HAVE_NLOHMANN_JSON
#  include <nlohmann/json.hpp>
#endif

#include "game_multiplayer.h"
#include "../output.h"
#include "../game_player.h"
#include "../sprite_character.h"
#include "../window_base.h"
#include "../drawable_mgr.h"
#include "../scene.h"
#include "../audio.h"
#include "../bitmap.h"
#include "../font.h"
#include "../input.h"
#include "../game_map.h"
#include "../game_party.h"
#include "../game_system.h"
#include "../game_screen.h"
#include "../game_switches.h"
#include "../game_variables.h"
#include "../battle_animation.h"
#include "../player.h"
#include "../cache.h"
#include "../string_view.h"
#include "../tone.h"
#include "../utils.h"
#include "../graphics.h"
#include "../debugtext_overlay.h"
#include "client_connection.h"
#include "chatui.h"
#include "nametag.h"
#include "game_playerother.h"
#include "playerother.h"
#include "messages.h"
#include "output_mt.h"
#include "util/serialize.h"

#ifndef EMSCRIPTEN
#  include "server.h"
#else
#  include <emscripten/eventloop.h>
#endif

using namespace Multiplayer;
using namespace Messages;

namespace {
	/* Due to creating a certain number of pictures based on the value of the picture id,
	 *  it is necessary to limit the maximum value of this id. See: Game_Pictures::GetPicture()
	 */
	const int picture_client_limit = 100;
	const int picture_limit = 50;

	// Config
	DebugTextMode dto_mode = DT_NONE; // debugtext overlay
	std::shared_ptr<int> multiplayer_json_request_id;
	std::string game_name;
	uint32_t client_hash;
	Game_ConfigMultiplayer cfg;
	int update_counter = 0;
	struct {
		bool enable_sounds{ true };
		bool mute_audio{ false };
		int moving_queue_limit{ 4 };
	} settings;

	// Connection
	ClientConnection connection;
	bool reconnect_wait{ false };
	bool active{ false }; // if true, it will automatically reconnect when disconnected
#ifdef EMSCRIPTEN
	long heartbeat_setinterval_id = 0;
#endif

	/**
	 * Sync
	 */

	bool switching_room{ true }; // when client enters new room, but not synced to server
	bool switched_room{ false }; // determines whether new connected players should fade in
	int room_id{-1};
	std::map<int, std::string> global_players_system;
	std::map<int, PlayerOther> players;
	std::vector<PlayerOther> fadeout_players;
	std::shared_ptr<int> sys_graphic_request_id;

	// Picture
	std::map<int, bool> sync_picture_cache;
	std::vector<std::string> global_sync_picture_names;
	std::vector<std::string> global_sync_picture_prefixes;

	// Battle
	std::vector<int> sync_battle_anim_ids;

	// Flash
	int frame_index{-1};
	int last_flash_frame_index{-1};
	std::unique_ptr<std::array<int, 5>> last_frame_flash;
	std::map<int, std::array<int, 5>> repeating_flashes;

	// Virtual3D
	struct Virtual3DMapConfig {
		int character_event_id;
		int character_terrain_id;
		int refresh_switch_id;
	};
	std::map<std::tuple<int8_t, int16_t, int16_t>, uint8_t> players_pos_cache; // {type, x, y} = any
	std::map<int, Virtual3DMapConfig> virtual_3d_map_configs;
	bool virtual_3d_entered = false;

	/**
	 * unused
	 */

	std::vector<int> sync_switches;
	std::vector<int> sync_vars;
	std::vector<int> sync_events;
	std::vector<int> sync_action_events;
	std::vector<std::string> sync_picture_names; // for badge conditions
} // end of namespace

namespace {

int GetPictureLimit() {
	auto num_pics = Game_Pictures::GetDefaultNumberOfPictures();
	if (!num_pics || num_pics > picture_limit) num_pics = picture_limit;
	return num_pics;
}

// player_id and picture_id starts at 1
int GetPlayerPictureId(int player_id, int picture_id) {
	auto pic_limit = GetPictureLimit();
	// the picture_id will be used as the size, so when used for
	//  indexing in Game_Pictures::GetPicture(), it will be -1
	return ((player_id - 1) % picture_client_limit + 1) * pic_limit
			+ ((picture_id - 1) % pic_limit + 1);
}

void UpdateClientHash() {
	std::stringstream ss;
	ss << connection.GetCryptKey() << game_name;
	client_hash = Utils::CRC32(ss);
}

int GetNumHash(int num) {
	if (connection.Encrypted()) {
		std::stringstream ss;
		WriteU32(ss, client_hash);
		WriteU32(ss, num);
		return Utils::CRC32(ss);
	}
	return num;
}

void Setup() {
#ifdef HAVE_NLOHMANN_JSON
	auto GetNumber = [](const nlohmann::json& obj, auto& var, const std::string& name) {
		if (obj.contains(name) && obj[name].is_number()) var = obj[name].get<double>();
	};
	auto LoadTextConfig = [&]() {
		Filesystem_Stream::InputStream is = FileFinder::OpenText("multiplayer.json");
		if (!is) return;

		const nlohmann::json& cfg = nlohmann::json::parse(is, nullptr, false);
		if (cfg.is_discarded()) return;

		if (!cfg.is_object()) return;

		if (cfg.contains("name") && cfg["name"].is_string()) {
			game_name = cfg["name"].get<std::string>();
			UpdateClientHash();
		}
		if (cfg.contains("sync") && cfg["sync"].is_object()) {
			const nlohmann::json& obj = cfg["sync"];
			if (obj.contains("picture_names") && obj["picture_names"].is_array()) {
				for (const auto& value : obj["picture_names"]) {
					if (!value.is_string()) break;
					global_sync_picture_names.emplace_back(value.get<std::string>());
				}
			}
			if (obj.contains("picture_prefixes") && obj["picture_prefixes"].is_array()) {
				for (const auto& value : obj["picture_prefixes"]) {
					if (!value.is_string()) break;
					global_sync_picture_prefixes.emplace_back(value.get<std::string>());
				}
			}
			if (obj.contains("virtual_3d_maps") && obj["virtual_3d_maps"].is_array()) {
				for (const auto& obj : obj["virtual_3d_maps"]) {
					if (!obj.is_object()) continue;
					int map_id{-1}, event_id{-1}, terrain_id{-1}, switch_id{-1};
					GetNumber(obj, map_id, "map_id");
					GetNumber(obj, event_id, "event_id");
					GetNumber(obj, terrain_id, "terrain_id");
					GetNumber(obj, switch_id, "switch_id");
					virtual_3d_map_configs[map_id] = { event_id, terrain_id, switch_id };
				}
			}
		}
		if (cfg.contains("debugtextoverlay") && cfg["debugtextoverlay"].is_object()) {
			const nlohmann::json& obj = cfg["debugtextoverlay"];
			if (obj.contains("color") && obj["color"].is_number())
				Graphics::GetDebugTextOverlay().SetColor(obj["color"].get<int>());
		}
		if (cfg.contains("chatui") && cfg["chatui"].is_object()) {
			const nlohmann::json& obj = cfg["chatui"];
			ChatUiTextConfig cfg;
			GetNumber(obj, cfg.color_status_connection, "color_status_connection");
			GetNumber(obj, cfg.color_status_room, "color_status_room");
			GetNumber(obj, cfg.color_log_divider, "color_log_divider");
			GetNumber(obj, cfg.color_log_name, "color_log_name");
			GetNumber(obj, cfg.color_log_visibility, "color_log_visibility");
			GetNumber(obj, cfg.color_log_room, "color_log_room");
			GetNumber(obj, cfg.color_log_time, "color_log_time");
			GetNumber(obj, cfg.color_log_message, "color_log_message");
			GetNumber(obj, cfg.color_log_truncatechar, "color_log_truncatechar");
			GetNumber(obj, cfg.color_typebox, "color_typebox");
			GetNumber(obj, cfg.color_print_message, "color_print_message");
			GetNumber(obj, cfg.color_print_label, "color_print_label");
			GetNumber(obj, cfg.color_print_label_message, "color_print_label_message");
			CUI().SetTextConfig(cfg);
		}
	};
	if (Player::game_config.engine != Player::EngineNone) {
		game_name = Player::game_title; // Try to give a name, but it may not be effective
		UpdateClientHash();

#ifndef EMSCRIPTEN
		LoadTextConfig();
#else
		auto* request = AsyncHandler::RequestFile("Text", "multiplayer");
		multiplayer_json_request_id = request->Bind([&](FileRequestResult* result) {
			if (result->success) LoadTextConfig();
		});
		request->SetImportantFile(true); // Continue the scene after waiting
		request->Start();
#endif // else EMSCRIPTEN
	}
#endif // ifdef HAVE_NLOHMANN_JSON
}

/**
 * Why did DrawableMgr::SetLocalList(Map Scene) && DrawableMgr::SetLocalList(Old Scene)
 *  When switching scenes, for example, by pressing ESC, the current scene will change.
 *  So, bring up the Map Scene first
 */
void SpawnOtherPlayer(int id) {
	auto& player = Main_Data::game_player;
	auto& nplayer = players[id].ch;
	nplayer.reset(new Game_PlayerOther(id));
	nplayer->SetSpriteGraphic(player->GetSpriteName(), player->GetSpriteIndex());
	nplayer->SetMoveSpeed(player->GetMoveSpeed());
	nplayer->SetMoveFrequency(player->GetMoveFrequency());
	nplayer->SetThrough(true);
	nplayer->SetLayer(player->GetLayer());
	nplayer->SetMultiplayerVisible(false);
	nplayer->SetBaseOpacity(0);

	auto scene_map = Scene::Find(Scene::SceneType::Map);
	if (!scene_map) {
		Output::Error("MP: unexpected, {}:{}", __FILE__, __LINE__);
		return;
	}
	auto old_list = &DrawableMgr::GetLocalList();
	DrawableMgr::SetLocalList(&scene_map->GetDrawableList());
	auto& sprite = players[id].sprite;
	sprite = std::make_unique<Sprite_Character>(nplayer.get());
	sprite->SetTone(Main_Data::game_screen->GetTone());
	DrawableMgr::SetLocalList(old_list);
}

// this assumes that the player is stopped
// return true if player moves normally, false if players teleports
bool MovePlayerToPos(Game_PlayerOther& player, int x, int y) {
	if (!player.IsStopping()) {
		Output::Error("MP: MovePlayerToPos unexpected error: the player is busy being animated");
	}
	int dx = x - player.GetX();
	int dy = y - player.GetY();
	int adx = abs(dx);
	int ady = abs(dy);
	if (Game_Map::LoopHorizontal() && adx == Game_Map::GetTilesX() - 1) {
		dx = dx > 0 ? -1 : 1;
		adx = 1;
	}
	if (Game_Map::LoopVertical() && ady == Game_Map::GetTilesY() - 1) {
		dy = dy > 0 ? -1 : 1;
		ady = 1;
	}
	if (dx == 0 && dy == 0) {
		player.SetX(x);
		player.SetY(y);
		return true;
	}
	if (adx > 1 || ady > 1 || !player.IsMultiplayerVisible()) {
		player.SetX(x);
		player.SetY(y);
		return false;
	}
	using D = Game_Character::Direction;
	constexpr int dir[3][3]{
		{D::UpLeft,   D::Up,   D::UpRight},
		{D::Left,     0,       D::Right},
		{D::DownLeft, D::Down, D::DownRight},
	};
	player.Move(dir[dy+1][dx+1]);
	return true;
}

struct PlayerData {
	int pos_type;
	int pos_x, pos_y;
	int speed;
	std::string sprite_name;
	int sprite_index;
	int facing;
	int transparency;
	bool hidden;
	std::string system_name;
};

PlayerData GetPlayerData() {
	const auto& player = Main_Data::game_player;
	PlayerData d;
	d.pos_type = 0;
	d.pos_x = player->GetX();
	d.pos_y = player->GetY();
	if (virtual_3d_entered) {
		auto cfg_it = virtual_3d_map_configs.find(room_id);
		if (cfg_it != virtual_3d_map_configs.end() && cfg_it->second.character_event_id != -1) {
			Game_Character* ch = Game_Map::GetEvent(cfg_it->second.character_event_id);
			if (ch) {
				d.pos_type = 1;
				d.pos_x = ch->GetX();
				d.pos_y = ch->GetY();
			}
		}
	}
	d.speed = player->GetMoveSpeed();
	d.sprite_name = player->GetSpriteName();
	d.sprite_index = player->GetSpriteIndex();
	d.facing = player->GetFacing();
	d.transparency = player->GetTransparency();
	d.hidden = player->IsSpriteHidden();
	d.system_name = ToString(Main_Data::game_system->GetSystemName());
	return d;
}

std::string GetDebugText(DebugTextMode mode) {
	PlayerData d = GetPlayerData();
	std::string sprite_name = std::move(d.sprite_name);
	if (sprite_name.empty()) sprite_name = "/";
	std::ostringstream os;
	if (mode & DT_DEFAULT) {
		os << "client hash: " << client_hash
			<< " | game: " << game_name
			<< " | picture names: " << global_sync_picture_names.size()
			<< " | picture prefixes: " << global_sync_picture_prefixes.size()
			<< " | virtual 3d maps: " << virtual_3d_map_configs.size();
	}
	if (mode & DT_PLAYER_A) {
		if (mode & DT_DEFAULT) os << " | ";
		os << "map id: " << room_id
			<< " | pos: (" << d.pos_x
			<< ", " << d.pos_y
			<< ")";
	}
	if (mode & DT_PLAYER_B) {
		if (mode & (DT_DEFAULT | DT_PLAYER_A)) os << " | ";
		os << "facing: " << d.facing
			<< " | speed: " << d.speed
			<< " | transparency: " << d.transparency
			<< " | hidden: " << d.hidden;
		os << " | sprite: (" << sprite_name
			<< ", " << d.sprite_index
			<< ") | system: " << d.system_name;
	}
	return os.str();
}

void SendBasicData() {
	connection.SendPacketAsync<NamePacket>(cfg.client_chat_name.Get());
	connection.SendPacketAsync<RoomPacket>(connection.Encrypted() ? 0 : room_id, GetNumHash(room_id));

	PlayerData d = GetPlayerData();
	connection.SendPacketAsync<MovePacket>(d.pos_type, d.pos_x, d.pos_y);
	connection.SendPacketAsync<SpeedPacket>(d.speed);
	connection.SendPacketAsync<SpritePacket>(d.sprite_name, d.sprite_index);
	if (d.facing > 0) {
		connection.SendPacketAsync<FacingPacket>(d.facing);
	}
	connection.SendPacketAsync<TransparencyPacket>(d.transparency);
	connection.SendPacketAsync<HiddenPacket>(d.hidden);
	connection.SendPacketAsync<SystemPacket>(d.system_name);
}

void ResetRepeatingFlash() {
	frame_index = -1;
	last_flash_frame_index = -1;
	last_frame_flash.reset();
	repeating_flashes.clear();
}

void Reset() {
	players.clear();
	players_pos_cache.clear();
	fadeout_players.clear();
	sync_switches.clear();
	sync_vars.clear();
	sync_events.clear();
	sync_action_events.clear();
	ResetRepeatingFlash();
	if (Main_Data::game_pictures) {
		// Erase all pictures
		auto start = GetPlayerPictureId(1, 1);
		auto end = GetPlayerPictureId(picture_client_limit, GetPictureLimit());
		Main_Data::game_pictures->EraseRange(start, end);
	}
	if (virtual_3d_entered) {
		auto cfg_it = virtual_3d_map_configs.find(room_id);
		if (cfg_it != virtual_3d_map_configs.end() && cfg_it->second.refresh_switch_id != -1)
			Main_Data::game_switches->Flip(cfg_it->second.refresh_switch_id);
	}
}

bool IsPictureSynced(int pic_id, Game_Pictures::ShowParams& params) {
	bool picture_synced = false;

	for (auto& picture_name : global_sync_picture_names) {
		if (picture_name == params.name) {
			picture_synced = true;
			break;
		}
	}

	if (!picture_synced) {
		for (auto& picture_prefix : global_sync_picture_prefixes) {
			std::string name_lower = params.name;
			std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), [](unsigned char c) { return std::tolower(c); });
			if (name_lower.rfind(picture_prefix, 0) == 0) {
				picture_synced = true;
				break;
			}
		}
	}

	sync_picture_cache[pic_id] = picture_synced;

	if (!picture_synced) {
		for (auto& picture_name : sync_picture_names) {
			if (picture_name == params.name) {
				picture_synced = true;
				break;
			}
		}
	}

	return picture_synced;
}

void InitConnection() {
	connection.SetRoomPacketType(RoomPacket::packet_type);

	using SystemMessage = ClientConnection::SystemMessage;
	using Connection = Multiplayer::Connection;

	connection.RegisterSystemHandler(SystemMessage::OPEN, [](Connection& _) {
		SendBasicData();
		connection.SendPacket(ClientHelloPacket(client_hash, room_id, cfg.client_chat_name.Get()));
		CUI().SetStatusConnection(true);
	});
	connection.RegisterSystemHandler(SystemMessage::CLOSE, [](Connection& _) {
		CUI().SetStatusConnection(false);
		if (active) {
			Output::Debug("MP: connection is closed");
			if (reconnect_wait) return;
			reconnect_wait = true;
#ifndef EMSCRIPTEN
			std::thread([]() {
				std::this_thread::sleep_for(std::chrono::seconds(3));
				reconnect_wait = false;
				if (active) {
					OutputMt::Info("MP: reconnecting: ID={}", room_id);
					GMI().Connect();
				}
			}).detach();
#else
			emscripten_set_timeout([](void* userData) {
				reconnect_wait = false;
				if (active) {
					Output::Info("MP: reconnecting: ID={}", room_id);
					GMI().Connect();
				}
			}, 3000, nullptr);
#endif
		}
	});
	connection.RegisterSystemHandler(SystemMessage::TERMINATED, [](Connection& _) {
		CUI().GotSystemMessage("!! Connection terminated");
		// Here only changes state, connection already disconnected
		GMI().Disconnect();
	});

	auto SetGlobalPlayersSystem = [](int id, const std::string& sys_name, bool force_update = false) {
		auto Update = [id, sys_name, force_update]() {
			auto it = global_players_system.find(id);
			// forced update is because system_pkt arrived earlier than name_pkt
			if (!force_update && it != global_players_system.end()) {
				if (it->second == sys_name)
					return;
			}
			global_players_system[id] = sys_name;
			if (players.find(id) == players.end()) return;
			auto& player = players[id];
			auto name_tag = player.name_tag.get();
			if (name_tag) {
				name_tag->SetSystemGraphic(sys_name);
			}
		};
#ifndef EMSCRIPTEN
		Update();
#else
		// AsyncHandler remembers downloaded files, see IsReady() and ClearRequests()
		FileRequestAsync* request = AsyncHandler::RequestFile("System", sys_name);
		sys_graphic_request_id = request->Bind([Update](FileRequestResult* result) {
			if (!result->success) {
				return;
			}
			Update();
		});
		request->SetGraphicFile(true);
		request->Start();
#endif
	};

	connection.RegisterHandler<RoomPacket>([](RoomPacket& p) {
		if (p.room_id_hash != GetNumHash(room_id)) {
			GMI().SwitchRoom(room_id); // wrong room, resend
			return;
		}
		// server syned. accept other players spawn
		switching_room = false;
	});
	connection.RegisterHandler<JoinPacket>([](JoinPacket& p) {
		// I am entering a new room and don't care about players in the old(server side) room
		if (switching_room)
			return;
		if (players.find(p.id) == players.end())
			SpawnOtherPlayer(p.id);
	});
	connection.RegisterHandler<LeavePacket>([](LeavePacket& p) {
		{
			auto it = global_players_system.find(p.id);
			if (it != global_players_system.end())
				global_players_system.erase(it);
		}
		auto it = players.find(p.id);
		if (it == players.end()) return;
		auto& player = it->second;
		if (player.name_tag) {
			auto scene_map = Scene::Find(Scene::SceneType::Map);
			if (!scene_map) {
				Output::Error("MP: unexpected, {}:{}", __FILE__, __LINE__);
				return;
			}
			auto old_list = &DrawableMgr::GetLocalList();
			DrawableMgr::SetLocalList(&scene_map->GetDrawableList());
			player.name_tag.reset();
			DrawableMgr::SetLocalList(old_list);
		}
		if (virtual_3d_entered) {
			auto cfg_it = virtual_3d_map_configs.find(room_id);
			if (cfg_it != virtual_3d_map_configs.end()) {
				for (const auto& it : player.previous_pos) {
					players_pos_cache.erase(it.second);
					if (cfg_it->second.refresh_switch_id != -1 && it.first == 1)
						Main_Data::game_switches->Flip(cfg_it->second.refresh_switch_id);
				}
			}
		}
		fadeout_players.emplace_back(std::move(player));
		players.erase(it);
		repeating_flashes.erase(p.id);
		if (Main_Data::game_pictures) {
			// Erase pictures for a player
			auto start = GetPlayerPictureId(p.id + 1, 1);
			auto end = GetPlayerPictureId(p.id + 1, GetPictureLimit());
			Main_Data::game_pictures->EraseRange(start, end);
		}
	});
	connection.RegisterHandler<ChatPacket>([SetGlobalPlayersSystem](ChatPacket& p) {
		if (p.type == 0)
			CUI().GotSystemMessage(p.message, p.visibility);
		else if (p.type == 1) {
			if (p.sys_name != "")
				SetGlobalPlayersSystem(p.id, p.sys_name);
			CUI().GotMessage(p.visibility, p.room_id, p.name, p.message, p.sys_name);
		}
	});
	connection.RegisterHandler<MovePacket>([](MovePacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		int x = Utils::Clamp((int)p.x, 0, Game_Map::GetTilesX() - 1);
		int y = Utils::Clamp((int)p.y, 0, Game_Map::GetTilesY() - 1);
		player.mvq.emplace_back(p.type, x, y);
	});
	connection.RegisterHandler<JumpPacket>([](JumpPacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		int x = Utils::Clamp((int)p.x, 0, Game_Map::GetTilesX() - 1);
		int y = Utils::Clamp((int)p.y, 0, Game_Map::GetTilesY() - 1);
		auto rc = player.ch->Jump(x, y);
		if (rc) {
			player.ch->SetMaxStopCount(player.ch->GetMaxStopCountForStep(player.ch->GetMoveFrequency()));
		}
	});
	connection.RegisterHandler<FacingPacket>([](FacingPacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		int facing = Utils::Clamp((int)p.facing, 0, 3);
		player.ch->SetFacing(facing);
	});
	connection.RegisterHandler<SpeedPacket>([](SpeedPacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		int speed = Utils::Clamp((int)p.speed, 1, 6);
		player.ch->SetMoveSpeed(speed);
	});
	connection.RegisterHandler<SpritePacket>([](SpritePacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		int idx = Utils::Clamp((int)p.index, 0, 7);
		player.ch->SetSpriteGraphic(std::string(p.name), idx);
	});
	connection.RegisterHandler<FlashPacket>([](FlashPacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		player.ch->Flash(p.r, p.g, p.b, p.p, p.f);
	});
	connection.RegisterHandler<RepeatingFlashPacket>([](RepeatingFlashPacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		auto flash_array = std::array<int, 5>{ p.r, p.g, p.b, p.p, p.f };
		repeating_flashes[p.id] = std::array<int, 5>(flash_array);
		player.ch->Flash(p.r, p.g, p.b, p.p, p.f);
	});
	connection.RegisterHandler<RemoveRepeatingFlashPacket>([](RemoveRepeatingFlashPacket& p) {
		if (players.find(p.id) == players.end()) return;
		repeating_flashes.erase(p.id);
	});
	connection.RegisterHandler<TransparencyPacket>([](TransparencyPacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		player.ch->SetTransparency(Utils::Clamp((int)p.transparency, 0, 7));
	});
	connection.RegisterHandler<HiddenPacket>([](HiddenPacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		player.ch->SetSpriteHidden(p.is_hidden);
	});
	connection.RegisterHandler<SystemPacket>([SetGlobalPlayersSystem](SystemPacket& p) {
		SetGlobalPlayersSystem(p.id, p.name, true);
	});
	connection.RegisterHandler<SoundEffectPacket>([](SoundEffectPacket& p) {
		if (players.find(p.id) == players.end()) return;
		if (settings.enable_sounds) {
			auto& player = players[p.id];

			int px = Main_Data::game_player->GetX();
			int py = Main_Data::game_player->GetY();
			int ox = player.ch->GetX();
			int oy = player.ch->GetY();

			int hmw = Game_Map::GetTilesX() / 2;
			int hmh = Game_Map::GetTilesY() / 2;

			int rx;
			int ry;

			if (Game_Map::LoopHorizontal() && px - ox >= hmw) {
				rx = Game_Map::GetTilesX() - (px - ox);
			} else if (Game_Map::LoopHorizontal() && px - ox < hmw * -1) {
				rx = Game_Map::GetTilesX() + (px - ox);
			} else {
				rx = px - ox;
			}

			if (Game_Map::LoopVertical() && py - oy >= hmh) {
				ry = Game_Map::GetTilesY() - (py - oy);
			} else if (Game_Map::LoopVertical() && py - oy < hmh * -1) {
				ry = Game_Map::GetTilesY() + (py - oy);
			} else {
				ry = py - oy;
			}

			int dist = std::sqrt(rx * rx + ry * ry);
			float dist_volume = 75.0f - ((float)dist * 10.0f);
			float sound_volume_multiplier = float(p.snd.volume) / 100.0f;
			int real_volume = std::max((int)(dist_volume * sound_volume_multiplier), 0);

			lcf::rpg::Sound sound;
			sound.name = p.snd.name;
			sound.volume = real_volume;
			sound.tempo = p.snd.tempo;
			sound.balance = p.snd.balance;

			Main_Data::game_system->SePlay(sound);
		}
	});

	auto pic_modify_args = [](PicturePacket& p) {
		if (Game_Map::LoopHorizontal()) {
			int alt_map_x = p.map_x + Game_Map::GetTilesX() * TILE_SIZE * TILE_SIZE;
			if (std::abs(p.map_x - Game_Map::GetPositionX()) > std::abs(alt_map_x - Game_Map::GetPositionX())) {
				p.map_x = alt_map_x;
			}
		}
		if (Game_Map::LoopVertical()) {
			int alt_map_y = p.map_y + Game_Map::GetTilesY() * TILE_SIZE * TILE_SIZE;
			if (std::abs(p.map_y - Game_Map::GetPositionY()) > std::abs(alt_map_y - Game_Map::GetPositionY())) {
				p.map_y = alt_map_y;
			}
		}
		p.params.position_x += (int)(
			std::floor((p.map_x / TILE_SIZE) - (p.pan_x / (TILE_SIZE * 2)))
			- std::floor((Game_Map::GetPositionX() / TILE_SIZE)
			- Main_Data::game_player->GetPanX() / (TILE_SIZE * 2))
		);
		p.params.position_y += (int)(
			std::floor((p.map_y / TILE_SIZE) - (p.pan_y / (TILE_SIZE * 2)))
			- std::floor((Game_Map::GetPositionY() / TILE_SIZE)
			- Main_Data::game_player->GetPanY() / (TILE_SIZE * 2)));
	};
	connection.RegisterHandler<ShowPicturePacket>([pic_modify_args](ShowPicturePacket& p) {
		if (players.find(p.id) == players.end()) return;
		pic_modify_args(p);
		int pic_id = GetPlayerPictureId(p.id + 1, p.pic_id);
		Main_Data::game_pictures->Show(pic_id, p.params);
	});
	connection.RegisterHandler<MovePicturePacket>([pic_modify_args](MovePicturePacket& p) {
		if (players.find(p.id) == players.end()) return;
		pic_modify_args(p);
		int pic_id = GetPlayerPictureId(p.id + 1, p.pic_id);
		Main_Data::game_pictures->Move(pic_id, p.params);
	});
	connection.RegisterHandler<ErasePicturePacket>([](ErasePicturePacket& p) {
		if (players.find(p.id) == players.end()) return;
		int pic_id = GetPlayerPictureId(p.id + 1, p.pic_id);
		Main_Data::game_pictures->Erase(pic_id);
	});

	connection.RegisterHandler<ShowPlayerBattleAnimPacket>([](ShowPlayerBattleAnimPacket& p) {
		if (players.find(p.id) == players.end()) return;
		const lcf::rpg::Animation* anim = lcf::ReaderUtil::GetElement(lcf::Data::animations, p.anim_id);
		if (anim) {
			players[p.id].battle_animation.reset(new BattleAnimationMap(*anim, *players[p.id].ch, false, true, true));
		} else {
			players[p.id].battle_animation.reset();
		}
	});
	connection.RegisterHandler<NamePacket>([](NamePacket& p) {
		if (players.find(p.id) == players.end()) return;
		auto& player = players[p.id];
		auto scene_map = Scene::Find(Scene::SceneType::Map);
		if (!scene_map) {
			Output::Error("MP: unexpected, {}:{}", __FILE__, __LINE__);
			return;
		}
		auto old_list = &DrawableMgr::GetLocalList();
		if (player.name_tag) {
			player.name_tag->SetNickname(p.name);
		} else {
			DrawableMgr::SetLocalList(&scene_map->GetDrawableList());
			player.name_tag = std::make_unique<NameTag>(p.id, p.name, player);
			DrawableMgr::SetLocalList(old_list);
		}
	});
}

} // end of namespace

/**
 * External access
 */

static Game_Multiplayer _instance;

Game_Multiplayer& Game_Multiplayer::Instance() {
	return _instance;
}

Game_Multiplayer::Game_Multiplayer() {
	InitConnection();
}

std::string Game_Multiplayer::GetDebugText(DebugTextMode mode) {
	return ::GetDebugText(mode);
}

void Game_Multiplayer::ToggleDebugTextOverlayMode(DebugTextMode mode) {
	if (dto_mode == DT_NONE) {
		dto_mode = mode;
	} else {
		dto_mode = dto_mode == mode ? DT_NONE : mode;
	}
	if (dto_mode != DT_NONE)
		Graphics::GetDebugTextOverlay().ShowItem("00_player_info");
	else
		Graphics::GetDebugTextOverlay().RemoveItem("00_player_info");
}

/** Config */

void Game_Multiplayer::GameLoaded() {
	Setup();
}

void Game_Multiplayer::SetRemoteAddress(std::string address) {
	cfg.client_remote_address.Set(address);
	connection.SetAddress(cfg.client_remote_address.Get());
}

void Game_Multiplayer::SetConfig(const Game_ConfigMultiplayer& _cfg) {
	cfg = _cfg;
#ifndef EMSCRIPTEN
	Server().SetConfig(_cfg);
	if (cfg.server_auto_start.Get())
		Server().Start();
#endif
	connection.SetConfig(&cfg);

	// Heartbeat
	if (!cfg.no_heartbeats.Get()) {
		connection.RegisterHandler<HeartbeatPacket>([](HeartbeatPacket& p) {});
#ifndef EMSCRIPTEN
		std::thread([]() {
			while (true) {
				std::this_thread::sleep_for(std::chrono::seconds(3));
				if (active && connection.IsConnected()) {
					connection.SendPacket(HeartbeatPacket());
				}
			}
		}).detach();
#else
		heartbeat_setinterval_id = emscripten_set_interval([](void* userData) {
			if (active && connection.IsConnected()) {
				connection.SendPacket(HeartbeatPacket());
			}
		}, 3000, nullptr);
#endif
	}
}

Game_ConfigMultiplayer& Game_Multiplayer::GetConfig() {
	return cfg;
}

/** Connection */

bool Game_Multiplayer::IsActive() {
	return active;
}

void Game_Multiplayer::Connect() {
	if (connection.IsConnected()) return;
	connection.SetCryptKey(cfg.client_crypt_key.Get());
	if (connection.Encrypted()) {
		Output::InfoStr("Connection is encrypted.");
	}
	UpdateClientHash();
	active = true;
	std::string remote_address = cfg.client_remote_address.Get();
#ifndef EMSCRIPTEN
	if (remote_address.empty()) {
		remote_address = "127.0.0.1:6500";
	}
#endif
	connection.SetAddress(remote_address);
	if (!cfg.client_socks5_address.Get().empty())
		connection.SetSocks5Address(cfg.client_socks5_address.Get());
	CUI().SetStatusConnection(false, true);
	connection.Open();
	if (room_id != -1)
		SwitchRoom(room_id);
}

void Game_Multiplayer::Disconnect() {
	active = false;
	Reset();
	connection.Close();
	CUI().SetStatusConnection(false);
}

/** Chat */

void Game_Multiplayer::SetChatName(std::string chat_name) {
	if (chat_name.size() > 16) Output::InfoStr("Chat name too long.");
	cfg.client_chat_name.Set(chat_name);
	connection.SendPacket(NamePacket(cfg.client_chat_name.Get()));
}

std::string Game_Multiplayer::GetChatName() {
	return cfg.client_chat_name.Get();
}

void Game_Multiplayer::SendChatMessage(int visibility, std::string message, int crypt_key_hash) {
	auto p = ChatPacket(visibility, message, ToString(Main_Data::game_system->GetSystemName()));
	p.crypt_key_hash = crypt_key_hash;
	p.name = cfg.client_chat_name.Get() != "" ? cfg.client_chat_name.Get() : "<unknown>";
	p.room_id = room_id;
	connection.SendPacket(p);
}

/** Screen */

void Game_Multiplayer::ApplyScreenTone() {
	Tone tone = Main_Data::game_screen->GetTone();
	for (auto& p : players) {
		p.second.sprite->SetTone(tone);
		auto name_tag = p.second.name_tag.get();
		if (name_tag)
			name_tag->SetEffectsDirty();
	}
}

void Game_Multiplayer::ApplyFlash(int r, int g, int b, int power, int frames) {
	for (auto& p : players) {
		p.second.ch->Flash(r, g, b, power, frames);
		auto name_tag = p.second.name_tag.get();
		if (name_tag)
			name_tag->SetFlashFramesLeft(frames);
	}
}

void Game_Multiplayer::ApplyRepeatingFlashes() {
	for (auto& rf : repeating_flashes) {
		if (players.find(rf.first) != players.end()) {
			std::array<int, 5> flash_array = rf.second;
			players[rf.first].ch->Flash(flash_array[0], flash_array[1], flash_array[2], flash_array[3], flash_array[4]);
			auto name_tag = players[rf.first].name_tag.get();
			if (name_tag)
				name_tag->SetFlashFramesLeft(flash_array[4]);
		}
	}
}

/**
 * Sync
 */

void Game_Multiplayer::SwitchRoom(int map_id, bool from_save) {
#ifdef EMSCRIPTEN
	/* Automatic connection in a production environment may be necessary,
	 * and if the address is empty, it will auto retrieve the address */
	if (!cfg.client_auto_connect.Get() && cfg.client_remote_address.Get().empty()) {
		cfg.client_auto_connect.Set(true);
	}
#endif
	SetNametagMode(cfg.client_name_tag_mode.Get());
	CUI().SetStatusRoom(map_id);
	Output::Debug("MP: room_id=map_id={} from_save={}", map_id, from_save);
	room_id = map_id;
	if (!active) {
		bool auto_connect = cfg.client_auto_connect.Get();
		if (auto_connect) {
			active = true;
			GMI().Connect();
		}
		Output::Debug("MP: active={} auto_connect={}", active, auto_connect);
		return;
	}
	switching_room = true;
	if (!from_save) {
		switched_room = false;
	}
	Reset();
	if (connection.IsConnected())
		SendBasicData();
}

void Game_Multiplayer::MapQuit() {
	Output::Debug("MP: map quit");
	SetNametagMode(cfg.client_name_tag_mode.Get());
	Reset();
}

void Game_Multiplayer::Quit() {
#ifdef EMSCRIPTEN
	if (heartbeat_setinterval_id) {
		// onExit triggers only after all set_intervals are cleared
		emscripten_clear_interval(heartbeat_setinterval_id);
		heartbeat_setinterval_id = 0;
	}
#endif
	Disconnect();
}

void Game_Multiplayer::MainPlayerMoved(int dir) {
	auto& p = Main_Data::game_player;
	connection.SendPacketAsync<MovePacket>(0, p->GetX(), p->GetY());
	virtual_3d_entered = false;
}

void Game_Multiplayer::MainPlayerFacingChanged(int dir) {
	connection.SendPacketAsync<FacingPacket>(dir);
}

void Game_Multiplayer::MainPlayerChangedMoveSpeed(int spd) {
	connection.SendPacketAsync<SpeedPacket>(spd);
}

void Game_Multiplayer::MainPlayerChangedSpriteGraphic(std::string name, int index) {
	connection.SendPacketAsync<SpritePacket>(name, index);
}

void Game_Multiplayer::MainPlayerJumped(int x, int y) {
	auto& p = Main_Data::game_player;
	connection.SendPacketAsync<JumpPacket>(x, y);
}

void Game_Multiplayer::MainPlayerFlashed(int r, int g, int b, int p, int f) {
	std::array<int, 5> flash_array = std::array<int, 5>{ r, g, b, p, f };
	if (last_flash_frame_index == frame_index - 1 && (last_frame_flash.get() == nullptr || *last_frame_flash == flash_array)) {
		// During this period, RepeatingFlashPacket will only be sent once
		if (last_frame_flash.get() == nullptr) {
			last_frame_flash = std::make_unique<std::array<int, 5>>(flash_array);
			connection.SendPacketAsync<RepeatingFlashPacket>(r, g, b, p, f);
		}
	} else {
		connection.SendPacketAsync<FlashPacket>(r, g, b, p, f);
		last_frame_flash.reset();
	}
	last_flash_frame_index = frame_index;
}

void Game_Multiplayer::MainPlayerChangedTransparency(int transparency) {
	connection.SendPacketAsync<TransparencyPacket>(transparency);
}

void Game_Multiplayer::MainPlayerChangedSpriteHidden(bool hidden) {
	connection.SendPacketAsync<HiddenPacket>(hidden);
}

void Game_Multiplayer::MainPlayerTeleported(int map_id, int x, int y) {
	if (virtual_3d_entered)
		virtual_3d_entered = false;
	/* Sometimes the starting position is not as expected,
	 *  but is moved through teleportation again */
	connection.SendPacketAsync<MovePacket>(0, x, y);
}

void Game_Multiplayer::MainPlayerTriggeredEvent(int event_id, bool action) {
}

void Game_Multiplayer::SystemGraphicChanged(StringView sys) {
	CUI().Refresh();
	connection.SendPacketAsync<SystemPacket>(ToString(sys));
}

void Game_Multiplayer::SePlayed(const lcf::rpg::Sound& sound) {
	if (!Main_Data::game_player->IsMenuCalling()) {
		connection.SendPacketAsync<SoundEffectPacket>(sound);
	}
}

void Game_Multiplayer::SwitchSet(int switch_id, int value_bin) {
}

void Game_Multiplayer::VariableSet(int var_id, int value) {
}

/** Picture */

void Game_Multiplayer::PictureShown(int pic_id, Game_Pictures::ShowParams& params) {
	if (IsPictureSynced(pic_id, params)) {
		auto& p = Main_Data::game_player;
		connection.SendPacketAsync<ShowPicturePacket>(GetNumHash(pic_id), pic_id, params,
			Game_Map::GetPositionX(), Game_Map::GetPositionY(),
			p->GetPanX(), p->GetPanY());
	}
}

void Game_Multiplayer::PictureMoved(int pic_id, Game_Pictures::MoveParams& params) {
	if (sync_picture_cache.count(pic_id) && sync_picture_cache[pic_id]) {
		auto& p = Main_Data::game_player;
		connection.SendPacketAsync<MovePicturePacket>(GetNumHash(pic_id), pic_id, params,
			Game_Map::GetPositionX(), Game_Map::GetPositionY(),
			p->GetPanX(), p->GetPanY());
	}
}

void Game_Multiplayer::PictureErased(int pic_id) {
	if (sync_picture_cache.count(pic_id) && sync_picture_cache[pic_id]) {
		sync_picture_cache.erase(pic_id);
		connection.SendPacketAsync<ErasePicturePacket>(GetNumHash(pic_id), pic_id);
	}
}

/** Battle */

bool Game_Multiplayer::IsBattleAnimSynced(int anim_id) {
	bool anim_synced = false;

	for (auto& battle_anim_id : sync_battle_anim_ids) {
		if (battle_anim_id == anim_id) {
			anim_synced = true;
			break;
		}
	}

	return anim_synced;
}

void Game_Multiplayer::PlayerBattleAnimShown(int anim_id) {
	if (IsBattleAnimSynced(anim_id)) {
		connection.SendPacketAsync<ShowPlayerBattleAnimPacket>(anim_id);
	}
}

void Game_Multiplayer::ApplyPlayerBattleAnimUpdates() {
	for (auto& p : players) {
		if (p.second.battle_animation) {
			auto& ba = p.second.battle_animation;
			if (!ba->IsDone()) {
				ba->Update();
			}
			if (ba->IsDone()) {
				ba.reset();
			}
		}
	}
}

/** Virtual3D */

void Game_Multiplayer::EventLocationChanged(int event_id, int x, int y) {
	auto cfg_it = virtual_3d_map_configs.find(room_id);
	if (cfg_it != virtual_3d_map_configs.end()) {
		if (cfg_it->second.character_event_id != -1
				&& event_id == cfg_it->second.character_event_id) {
			connection.SendPacketAsync<MovePacket>(1, x, y);
			virtual_3d_entered = true;
		}
	}
}

int Game_Multiplayer::GetTerrainTag(int original_terrain_id, int x, int y) {
	if (virtual_3d_entered) {
		auto cfg_it = virtual_3d_map_configs.find(room_id);
		if (cfg_it != virtual_3d_map_configs.end()) {
			auto it = players_pos_cache.find(std::make_tuple(
				cfg_it->second.character_event_id != -1 ? 1 : 0, x, y));
			if (it != players_pos_cache.end())
				return it->second;
		}
	}
	return original_terrain_id;
}

/**
 * Steps
 */

void Game_Multiplayer::Update() {
	if (active) {
		connection.Receive();
	}
	if (dto_mode != DT_NONE) {
		Graphics::GetDebugTextOverlay().UpdateItem("00_player_info", ::GetDebugText(dto_mode));
	}
	OutputMt::Update();
}

void Game_Multiplayer::MapUpdate() {
	if (active) {
		// If Flash continues, last_flash_frame_index will always follow frame_index
		if (last_flash_frame_index > -1 && frame_index > last_flash_frame_index) {
			connection.SendPacketAsync<RemoveRepeatingFlashPacket>();
			last_flash_frame_index = -1;
			last_frame_flash.reset();
		}

		++frame_index;

		bool check_name_tag_overlap = frame_index % (8 + ((players.size() >> 4) << 3)) == 0;

		bool is_virtual_3d_map = false;
		bool virtual_3d_updated = false;
		int virtual_3d_character_terrain_id;
		int virtual_3d_refresh_switch_id;

		auto cfg_it = virtual_3d_map_configs.find(room_id);
		if (cfg_it != virtual_3d_map_configs.end()) {
			is_virtual_3d_map = true;
			virtual_3d_character_terrain_id = cfg_it->second.character_terrain_id;
			virtual_3d_refresh_switch_id = cfg_it->second.refresh_switch_id;
		}

		for (auto& p : players) {
			auto& q = p.second.mvq;
			auto& ch = p.second.ch;
			// if player moves too fast
			bool is_mvq_truncated = false;
			if (q.size() > settings.moving_queue_limit) {
				q.erase(
					q.begin(),
					std::next(q.begin(), q.size() - settings.moving_queue_limit)
				);
				is_mvq_truncated = true;
			}
			if (!q.empty() && is_virtual_3d_map) {
				auto [type, x, y] = q.front();
				auto it = p.second.previous_pos.find(type);
				if (it != p.second.previous_pos.end())
					players_pos_cache.erase(it->second);
				auto pos = std::make_tuple(type, x, y);
				if (players_pos_cache.size() < 100)
					players_pos_cache[pos] = virtual_3d_character_terrain_id;
				if (type > -1 && type < 2)
					p.second.previous_pos[type] = pos;
				virtual_3d_updated = true;
			}
			if (!q.empty() && ch->IsStopping()) {
				auto [_, x, y] = q.front();
				int prev_x{ch->GetX()}, prev_y{ch->GetY()};
				bool is_normal_move = MovePlayerToPos(*ch, x, y);
				if (switched_room) {
					// only for teleportation
					if (ch->IsMultiplayerVisible() && !is_mvq_truncated && !is_normal_move) {
						ch->SetBaseOpacity(0); // prepare to fade-in at new position
						PlayerOther shadow = p.second.GetCopy();
						shadow.ch->SetX(prev_x);
						shadow.ch->SetY(prev_y);
						fadeout_players.emplace_back(std::move(shadow));
					}
				} else { // when you enter the room
					ch->SetMultiplayerVisible(true);
					ch->SetBaseOpacity(32);
				}
				q.pop_front();
				// when others enter the room
				if (!ch->IsMultiplayerVisible()) {
					ch->SetMultiplayerVisible(true);
				}
			}
			/* !ch->IsSpriteHidden(): player enters the map, screen is black,
			 *  and hidden is true, until transition complete */
			if (ch->IsMultiplayerVisible() && !ch->IsSpriteHidden() && ch->GetBaseOpacity() < 32) {
				ch->SetBaseOpacity(ch->GetBaseOpacity() + 1);
			}
			ch->SetProcessed(false);
			ch->Update();
			p.second.sprite->Update();

			if (check_name_tag_overlap) {
				bool overlap = false;
				int x = ch->GetX();
				int y = ch->GetY();
				for (auto& p2 : players) {
					auto& ch2 = p2.second.ch;
					// are players and players overlapping?
					if (x == ch2->GetX()) {
						int y2 = ch2->GetY();
						if (y == 0) {
							if (Game_Map::LoopVertical() && y2 == Game_Map::GetTilesY() - 1) {
								overlap = true;
								break;
							}
						} else if (y2 == y - 1) {
							overlap = true;
							break;
						}
					}
				}
				if (!overlap) {
					auto& player = Main_Data::game_player;
					// do players overlap with you?
					if (x == player->GetX()) {
						if (y == 0) {
							if (Game_Map::LoopVertical() && player->GetY() == Game_Map::GetTilesY() - 1) {
								overlap = true;
							}
						} else if (player->GetY() == y - 1) {
							overlap = true;
						}
					}
				}
				auto name_tag = p.second.name_tag.get();
				if (name_tag)
					name_tag->SetTransparent(overlap);
			}
		}

		if (!switching_room && !switched_room) {
			switched_room = true;
		}

		if (virtual_3d_refresh_switch_id != -1 && virtual_3d_updated)
			Main_Data::game_switches->Flip(virtual_3d_refresh_switch_id);
	}

	if (!fadeout_players.empty()) {
		auto scene_map = Scene::Find(Scene::SceneType::Map);
		if (!scene_map) {
			Output::Error("MP: unexpected, {}:{}", __FILE__, __LINE__);
			return;
		}

		auto old_list = &DrawableMgr::GetLocalList();
		DrawableMgr::SetLocalList(&scene_map->GetDrawableList());

		for (auto fopi = fadeout_players.rbegin(); fopi != fadeout_players.rend();) {
			auto& ch = fopi->ch;
			if (ch->GetBaseOpacity() > 0) {
				ch->SetBaseOpacity(ch->GetBaseOpacity() - 1);
				ch->SetProcessed(false);
				ch->Update();
				fopi->sprite->Update();
				++fopi;
			} else {
				fopi = decltype(fopi)(fadeout_players.erase(fopi.base() - 1));
			}
		}

		DrawableMgr::SetLocalList(old_list);
	}

	if (connection.IsConnected())
		connection.FlushQueue();
}
