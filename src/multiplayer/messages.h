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

#ifndef EP_MESSAGES_H
#define EP_MESSAGES_H

#include "packet.h"

#ifndef SERVER
#  include <lcf/rpg/sound.h>
#  include "../game_pictures.h"
#endif

namespace Messages {
	enum PacketType : uint8_t {
		PT_HEARTBEAT = 0x01, PT_CLIENT_HELLO = 0x02,
		PT_ROOM = 0x22, PT_JOIN = 0x23, PT_LEAVE = 0x24,
		PT_NAME = 0x25, PT_CHAT = 0x26,
		PT_MOVE = 0x27, PT_JUMP = 0x28,
		PT_FACING= 0x2a, PT_SPEED = 0x2b, PT_SPRITE = 0x2c,
		PT_FLASH = 0x2d, PT_REPEATING_FLASH = 0x2e, PT_REMOVE_REPEATING_FLASH = 0x2f,
		PT_TRANSPARENCY = 0x30, PT_HIDDEN = 0x31, PT_SYSTEM = 0x32, PT_SOUND_EFFECT = 0x33,
		PT_SHOW_PICTURE = 0x34, PT_MOVE_PICTRUE = 0x35, PT_ERASE_PICTURE = 0x36,
		PT_SHOW_PLAYER_BATTLE_ANIM = 0x37,
	};

	using Packet = Multiplayer::Packet;

	/**
	 * Heartbeat
	 */

	class HeartbeatPacket : public Packet {
	public:
		constexpr static uint8_t packet_type{ PT_HEARTBEAT };
		HeartbeatPacket() : Packet(packet_type) {}
	};

	/**
	 * ClientHelloPacket
	 */

	class ClientHelloPacket : public Packet {
	public:
		constexpr static uint8_t packet_type{ PT_CLIENT_HELLO };
		ClientHelloPacket() : Packet(packet_type) {}
		ClientHelloPacket(uint32_t _ch, uint16_t _rid, std::string _n)
			: Packet(packet_type), client_hash(_ch), room_id(_rid), name(std::move(_n)) {}
		uint32_t client_hash{0};
		uint16_t room_id{0};
		std::string name;
	private:
		void Serialize(std::ostream& os) const override { WritePartial(os, client_hash); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, room_id, name); }
		void DeSerialize(std::istream& is) override { client_hash = ReadU32(is); }
		void DeSerialize2(std::istream& is) override { room_id = ReadU16(is), name = DeSerializeString16(is); }
	};

	/**
	 * Room
	 */

	class RoomPacket : public Packet {
	public:
		constexpr static uint8_t packet_type{ PT_ROOM };
		RoomPacket() : Packet(packet_type) {}
		RoomPacket(uint16_t _rid, uint32_t _ridh) : Packet(packet_type), room_id(_rid), room_id_hash(_ridh) {}
		uint16_t room_id{0};
		uint32_t room_id_hash{0};
	private:
		void Serialize(std::ostream& os) const override { WritePartial(os, room_id, room_id_hash); }
		void DeSerialize(std::istream& is) override { room_id = ReadU16(is), room_id_hash = ReadU32(is); }
	};

	/**
	 * Base Class: Player
	 */

	class PlayerPacket : public Packet {
	public:
		uint16_t id{0};
	protected:
		PlayerPacket(uint8_t _packet_type) : Packet(_packet_type) {} // C2S
		PlayerPacket(uint8_t _packet_type, uint16_t _id) : Packet(_packet_type), id(_id) {} // S2C
		void Serialize(std::ostream& os) const override { WritePartial(os, id); }
		void DeSerialize(std::istream& is) override { id = ReadU16(is); }
	};

	/**
	 * Join
	 */

	class JoinPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_JOIN };
		JoinPacket() : PlayerPacket(packet_type) {}
		JoinPacket(uint16_t _id) : PlayerPacket(packet_type, _id) {} // S2C
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
	};

	/**
	 * Leave
	 */

	class LeavePacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_LEAVE };
		LeavePacket() : PlayerPacket(packet_type) {}
		LeavePacket(uint16_t _id) : PlayerPacket(packet_type, _id) {} // S2C
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
	};

	/**
	 * Name
	 */

	class NamePacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_NAME };
		NamePacket() : PlayerPacket(packet_type) {}
		NamePacket(std::string _name) // C2S
			: PlayerPacket(packet_type), name(std::move(_name)) {}
		NamePacket(uint16_t _id, std::string _name) // S2C
			: PlayerPacket(packet_type, _id), name(std::move(_name)) {}
		std::string name;
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, name); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { name = DeSerializeString16(is); }
	};

	/**
	 * Chat
	 */

	class ChatPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_CHAT };
		ChatPacket() : PlayerPacket(packet_type) {}
		ChatPacket(uint8_t _v, std::string _m, std::string _s) // C2S
			: PlayerPacket(packet_type), visibility(_v), message(std::move(_m)), sys_name(std::move(_s)) {}
		ChatPacket(uint16_t _id, uint8_t _t, uint8_t _v, uint16_t _r, std::string _n, std::string _m) // S2C
			: PlayerPacket(packet_type, _id), type(_t), visibility(_v),
			room_id(_r), name(std::move(_n)), message(std::move(_m)) {}
		uint8_t type{0}; // 0: system, 1: chat
		uint8_t visibility{0};
		uint32_t crypt_key_hash{0};
		uint16_t room_id{0};
		std::string name;
		std::string message;
		std::string sys_name;
	private:
		void Serialize(std::ostream& os) const override {
			PlayerPacket::Serialize(os);
			WritePartial(os, type, visibility, crypt_key_hash);
		}
		void Serialize2(std::ostream& os) const override {
			WritePartial(os, room_id, name, message, sys_name);
		}
		void DeSerialize(std::istream& is) override {
			PlayerPacket::DeSerialize(is);
			type = ReadU8(is), visibility = ReadU8(is), crypt_key_hash = ReadU32(is);
		}
		void DeSerialize2(std::istream& is) override {
			room_id = ReadU16(is);
			name = DeSerializeString16(is);
			message = DeSerializeString16(is);
			sys_name = DeSerializeString16(is);
		}
	};

	/**
	 * Move
	 */

	class MovePacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_MOVE };
		MovePacket() : PlayerPacket(packet_type) {}
		MovePacket(int8_t _type, uint16_t _x, uint16_t _y) // C2S
			: PlayerPacket(packet_type), type(_type), x(_x), y(_y) {}
		MovePacket(uint16_t _id, int8_t _type, uint16_t _x, uint16_t _y) // S2C
			: PlayerPacket(packet_type, _id), type(_type), x(_x), y(_y) {}
		int8_t type{0}; // 0: normal, 1: event location
		uint16_t x{0}, y{0};
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, type, x, y); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override {
			type = ReadS8(is);
			x = ReadU16(is), y = ReadU16(is);
		}
	};

	/**
	 * Jump
	 */

	class JumpPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_JUMP };
		JumpPacket() : PlayerPacket(packet_type) {}
		JumpPacket(uint16_t _x, uint16_t _y) // C2S
			: PlayerPacket(packet_type), x(_x), y(_y) {}
		JumpPacket(uint16_t _id, uint16_t _x, uint16_t _y) // S2C
			: PlayerPacket(packet_type, _id), x(_x), y(_y) {}
		uint16_t x{0}, y{0};
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, x, y); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { x = ReadU16(is), y = ReadU16(is); }
	};

	/**
	 * Facing
	 */

	class FacingPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_FACING };
		FacingPacket() : PlayerPacket(packet_type) {}
		FacingPacket(uint8_t _facing) // C2S
			: PlayerPacket(packet_type), facing(_facing) {}
		FacingPacket(uint16_t _id, uint8_t _facing) // S2C
			: PlayerPacket(packet_type, _id), facing(_facing) {}
		uint8_t facing{0};
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, facing); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { facing = ReadU8(is); }
	};

	/**
	 * Speed
	 */

	class SpeedPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_SPEED };
		SpeedPacket() : PlayerPacket(packet_type) {}
		SpeedPacket(uint16_t _speed) // C2S
			: PlayerPacket(packet_type), speed(_speed) {}
		SpeedPacket(uint16_t _id, uint16_t _speed) // S2C
			: PlayerPacket(packet_type, _id), speed(_speed) {}
		uint16_t speed{0};
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, speed); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { speed = ReadU16(is); }
	};

	/**
	 * Sprite
	 */

	class SpritePacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_SPRITE };
		SpritePacket() : PlayerPacket(packet_type) {}
		SpritePacket(std::string _n, int16_t _i) // C2S
			: PlayerPacket(packet_type), name(std::move(_n)), index(_i) {}
		SpritePacket(uint16_t _id, std::string _n, int16_t _i) // S2C
			: PlayerPacket(packet_type, _id), name(std::move(_n)), index(_i) {}
		std::string name;
		int16_t index{-1};
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, name, index); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { name = DeSerializeString16(is), index = ReadS16(is); }
	};

	/**
	 * Base Class: Flash
	 */

	class FlashPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_FLASH };
		FlashPacket() : PlayerPacket(packet_type) {}
		FlashPacket(uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _p, uint8_t _f) // C2S
			: PlayerPacket(packet_type), r(_r), g(_g), b(_b), p(_p), f(_f) {}
		FlashPacket(uint16_t _id, uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _p, uint8_t _f) // S2C
			: PlayerPacket(packet_type, _id), r(_r), g(_g), b(_b), p(_p), f(_f) {}
		uint8_t r{0}, g{0}, b{0}, p{0}, f{0}; // p: power, f: frames
	protected:
		FlashPacket(uint8_t _packet_type) : PlayerPacket(_packet_type) {}
		FlashPacket(uint8_t _packet_type, uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _p, uint8_t _f) // C2S
			: PlayerPacket(_packet_type), r(_r), g(_g), b(_b), p(_p), f(_f) {}
		FlashPacket(uint8_t _packet_type, uint16_t _id, uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _p, uint8_t _f) // S2C
			: PlayerPacket(_packet_type, _id), r(_r), g(_g), b(_b), p(_p), f(_f) {}
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, r, g, b, p, f); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override {
			r = ReadU8(is), g = ReadU8(is), b = ReadU8(is);
			p = ReadU8(is), f = ReadU8(is);
		}
	};

	/**
	 * Repeating Flash
	 */

	class RepeatingFlashPacket : public FlashPacket {
	public:
		constexpr static uint8_t packet_type{ PT_REPEATING_FLASH };
		RepeatingFlashPacket() : FlashPacket(packet_type) {}
		RepeatingFlashPacket(uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _p, uint8_t _f) // C2S
			: FlashPacket(packet_type, _r, _g, _b, _p, _f) {}
		RepeatingFlashPacket(uint16_t _id, uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _p, uint8_t _f) // S2C
			: FlashPacket(packet_type, _id, _r, _g, _b, _p, _f) {}
	};

	/**
	 * Remove Repeating Flash
	 */

	class RemoveRepeatingFlashPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_REMOVE_REPEATING_FLASH };
		RemoveRepeatingFlashPacket() : PlayerPacket(packet_type) {} // C2S
		RemoveRepeatingFlashPacket(uint16_t _id) : PlayerPacket(packet_type, _id) {} // S2C
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
	};

	/**
	 * Transparency
	 */

	class TransparencyPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_TRANSPARENCY };
		TransparencyPacket() : PlayerPacket(packet_type) {}
		TransparencyPacket(uint8_t _tr)
			: PlayerPacket(packet_type), transparency(_tr) {} // C2S
		TransparencyPacket(uint16_t _id, uint8_t _tr) // S2C
			: PlayerPacket(packet_type, _id), transparency(_tr) {}
		uint8_t transparency{0};
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, transparency); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { transparency = ReadU8(is); }
	};

	/**
	 * Hidden
	 */

	class HiddenPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_HIDDEN };
		HiddenPacket() : PlayerPacket(packet_type) {}
		HiddenPacket(uint8_t _is_hidden) // C2S
			: PlayerPacket(packet_type), is_hidden(_is_hidden) {}
		HiddenPacket(uint16_t _id, uint8_t _is_hidden) // S2C
			: PlayerPacket(packet_type, _id), is_hidden(_is_hidden) {}
		bool is_hidden{false};
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, is_hidden); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { is_hidden = ReadU8(is); }
	};

	/**
	 * System Graphic
	 */

	class SystemPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_SYSTEM };
		SystemPacket() : PlayerPacket(packet_type) {}
		SystemPacket(std::string _name) // C2S
			: PlayerPacket(packet_type), name(std::move(_name)) {}
		SystemPacket(uint16_t _id, std::string _name) // S2C
			: PlayerPacket(packet_type, _id), name(std::move(_name)) {}
		std::string name;
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, name); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { name = DeSerializeString16(is); }
	};

	/**
	 * Sound Effect
	 */

#ifdef SERVER
	/** liblcf/src/generated/lcf/rpg/sound.h */
	namespace lcf {
	namespace rpg {
		struct Sound {
			std::string name = "(OFF)";
			int32_t volume = 100;
			int32_t tempo = 100;
			int32_t balance = 50;
		};
	} // namespace rpg
	} // namespace lcf
#endif

	class SoundEffectPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_SOUND_EFFECT };
		SoundEffectPacket() : PlayerPacket(packet_type) {}
		SoundEffectPacket(lcf::rpg::Sound _d) // C2S
			: PlayerPacket(packet_type), snd(std::move(_d)) {}
		SoundEffectPacket(uint16_t _id, lcf::rpg::Sound _d) // S2C
			: PlayerPacket(packet_type, _id), snd(std::move(_d)) {}
		lcf::rpg::Sound snd;
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override {
			WritePartial(os, snd.name, (uint16_t)snd.volume, (uint16_t)snd.tempo, (uint16_t)snd.balance);
		}
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override {
			snd.name = DeSerializeString16(is);
			snd.volume = ReadU16(is);
			snd.tempo = ReadU16(is);
			snd.balance = ReadU16(is);
		}
	};

	/**
	 * Base Class: Picture
	 */

#ifdef SERVER
	/** player/src/game_pictures.h */
	namespace Game_Pictures {
		struct Params {
			int position_x = 0;
			int position_y = 0;
			int top_trans = 0;
			int bottom_trans = 0;
			int red = 100;
			int green = 100;
			int blue = 100;
			int saturation = 100;
			int effect_mode = 0;
			int effect_power = 0;
			// Extensions
			bool flip_x = false;
			bool flip_y = false;
			int blend_mode = 0;
			int origin = 0;
			int magnify_width = 100; // RPG_RT supports magnify, but not independent for w/h
			int magnify_height = 100;
		};
		struct ShowParams : Params {
			std::string name;
			// RPG Maker 2k3 1.12
			int spritesheet_cols = 1;
			int spritesheet_rows = 1;
			int spritesheet_frame = 0;
			int spritesheet_speed = 0;
			int map_layer = 7;
			int battle_layer = 0;
			// erase_on_map_change | affected_by_flash | affected_by_shake
			int flags = 1 | 32 | 64;
			bool spritesheet_play_once = false;
			bool use_transparent_color = false;
			bool fixed_to_map = false;
		};
		struct MoveParams : Params {
			int duration = 0;
		};
	};
#endif

	class PicturePacket : public PlayerPacket {
	public:
		// Skip Game_Pictures::Params&, reference cannot be assigned
		PicturePacket& operator=(const PicturePacket& o) {
			Packet::SetPacketCrypt(o.GetPacketCrypt());
			id = o.id;
			pic_id_hash = o.pic_id_hash;
			if (!Packet::Encrypted()) {
				pic_id = o.pic_id;
				map_x = o.map_x; map_y = o.map_y;
				pan_x = o.pan_x; pan_y = o.pan_y;
			}
			return *this;
		}
		uint32_t pic_id_hash{0};
		uint16_t pic_id{0};
		int16_t map_x{0}, map_y{0};
		int16_t pan_x{0}, pan_y{0};
		Game_Pictures::Params& params;
	protected:
		PicturePacket(uint8_t _packet_type, uint32_t _pidh, uint16_t _pic_id, Game_Pictures::Params& _p, // C2S
				int16_t _mx, int16_t _my, int16_t _panx, int16_t _pany)
			: PlayerPacket(_packet_type), pic_id_hash(_pidh), pic_id(_pic_id), params(_p),
			map_x(_mx), map_y(_my), pan_x(_panx), pan_y(_pany) {}
		PicturePacket(uint8_t _packet_type, uint16_t _id, uint32_t _pidh, uint16_t _pic_id, Game_Pictures::Params& _p, // S2C
				int16_t _mx, int16_t _my, int16_t _panx, int16_t _pany)
			: PlayerPacket(_packet_type, _id), pic_id_hash(_pidh), pic_id(_pic_id), params(_p),
			map_x(_mx), map_y(_my), pan_x(_panx), pan_y(_pany) {}
		void Serialize(std::ostream& os) const override {
			PlayerPacket::Serialize(os);
			WritePartial(os, pic_id_hash);
		}
		void Serialize2(std::ostream& os) const override {
			WritePartial(os, pic_id, map_x, map_y, pan_x, pan_y,
				(int16_t)params.position_x, (int16_t)params.position_y,
				(int16_t)params.top_trans, (int16_t)params.bottom_trans,
				(uint8_t)params.red, (uint8_t)params.green, (uint8_t)params.blue, (uint8_t)params.saturation,
				(int16_t)params.effect_mode, (int16_t)params.effect_power,
				// Extensions
				(uint8_t)params.flip_x, (uint8_t)params.flip_y,
				(uint8_t)params.blend_mode, (int8_t)params.origin,
				(int16_t)params.magnify_width, (int16_t)params.magnify_height);
		}
		void DeSerialize(std::istream& is) override {
			PlayerPacket::DeSerialize(is);
			pic_id_hash = ReadU32(is);
		}
		void DeSerialize2(std::istream& is) override {
			pic_id = ReadU16(is);
			map_x = ReadS16(is), map_y = ReadS16(is);
			pan_x = ReadS16(is), pan_y = ReadS16(is);
		}
		static void BuildParams(Game_Pictures::Params& p, std::istream& is) {
			p.position_x = ReadS16(is), p.position_y = ReadS16(is);
			p.top_trans = ReadS16(is), p.bottom_trans = ReadS16(is);
			p.red = ReadU8(is), p.green = ReadU8(is), p.blue = ReadU8(is), p.saturation = ReadU8(is);
			p.effect_mode = ReadS16(is), p.effect_power = ReadS16(is);
			// Extensions
			p.flip_x = ReadU8(is), p.flip_y = ReadU8(is),
			p.blend_mode = ReadU8(is), p.origin = ReadS8(is),
			p.magnify_width = ReadS16(is), p.magnify_height = ReadS16(is);
		}
	};

	/**
	 * Show Picture
	 */

	class ShowPicturePacket : public PicturePacket {
	public:
		constexpr static uint8_t packet_type{ PT_SHOW_PICTURE };
		ShowPicturePacket() : PicturePacket(packet_type, 0, 0, params, 0, 0, 0, 0) {}
		ShowPicturePacket(uint32_t _pidh, uint16_t _pid, Game_Pictures::ShowParams _p, // C2S
				int16_t _mx, int16_t _my, int16_t _px, int16_t _py)
			: PicturePacket(packet_type, _pidh, _pid, params, _mx, _my, _px, _py), params(std::move(_p)) {}
		ShowPicturePacket(uint16_t _id, uint32_t _pidh, uint16_t _pid, Game_Pictures::ShowParams _p, // S2C
				int16_t _mx, int16_t _my, int16_t _px, int16_t _py)
			: PicturePacket(packet_type, _id, _pidh, _pid, params, _mx, _my, _px, _py), params(std::move(_p)) {}
		Game_Pictures::ShowParams params;
	private:
		void Serialize(std::ostream& os) const override { PicturePacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override {
			PicturePacket::Serialize2(os);
			WritePartial(os, params.name,
				(uint8_t)params.spritesheet_cols, (uint8_t)params.spritesheet_rows,
				(uint8_t)params.spritesheet_frame, (uint8_t)params.spritesheet_speed,
				(uint8_t)params.spritesheet_play_once,
				(uint8_t)params.map_layer, (uint8_t)params.battle_layer,
				(uint8_t)params.flags,
				(uint8_t)params.use_transparent_color, (uint8_t)params.fixed_to_map);
		}
		void DeSerialize(std::istream& is) override { PicturePacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override {
			PicturePacket::DeSerialize2(is);
			params = BuildParams(is);
		}
		Game_Pictures::ShowParams BuildParams(std::istream& is) const {
			Game_Pictures::ShowParams p;
			PicturePacket::BuildParams(p, is);
			p.name = DeSerializeString16(is);
			p.spritesheet_cols = ReadU8(is), p.spritesheet_rows = ReadU8(is);
			p.spritesheet_frame = ReadU8(is), p.spritesheet_speed = ReadU8(is);
			p.spritesheet_play_once = ReadU8(is);
			p.map_layer = ReadU8(is), p.battle_layer = ReadU8(is);
			p.flags = ReadU8(is);
			p.use_transparent_color = ReadU8(is), p.fixed_to_map = ReadU8(is);
			return p;
		}
	};

	/**
	 * Move Picture
	 */

	class MovePicturePacket : public PicturePacket {
	public:
		constexpr static uint8_t packet_type{ PT_MOVE_PICTRUE };
		MovePicturePacket() : PicturePacket(packet_type, 0, 0, params, 0, 0, 0, 0) {}
		MovePicturePacket(uint32_t _pidh, uint16_t _pid, Game_Pictures::MoveParams _p, // C2S
				int16_t _mx, int16_t _my, int16_t _px, int16_t _py)
			: PicturePacket(packet_type, _pidh, _pid, params, _mx, _my, _px, _py), params(std::move(_p)) {}
		MovePicturePacket(uint16_t _id, uint32_t _pidh, uint16_t _pid, Game_Pictures::MoveParams _p, // S2C
				int16_t _mx, int16_t _my, int16_t _px, int16_t _py)
			: PicturePacket(packet_type, _id, _pidh, _pid, params, _mx, _my, _px, _py), params(std::move(_p)) {}
		Game_Pictures::MoveParams params;
	private:
		void Serialize(std::ostream& os) const override { PicturePacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override {
			PicturePacket::Serialize2(os);
			WritePartial(os, (int16_t)params.duration);
		}
		void DeSerialize(std::istream& is) override { PicturePacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override {
			PicturePacket::DeSerialize2(is);
			params = BuildParams(is);
		}
		Game_Pictures::MoveParams BuildParams(std::istream& is) const {
			Game_Pictures::MoveParams p;
			PicturePacket::BuildParams(p, is);
			p.duration = ReadS16(is);
			return p;
		}
	};

	/**
	 * Erase Picture
	 */

	class ErasePicturePacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_ERASE_PICTURE };
		ErasePicturePacket() : PlayerPacket(packet_type) {}
		ErasePicturePacket(uint32_t _pidh, uint16_t _pid) // C2S
			: PlayerPacket(packet_type), pic_id_hash(_pidh), pic_id(_pid) {}
		ErasePicturePacket(uint16_t _id, uint32_t _pidh, uint16_t _pid) // S2C
			: PlayerPacket(packet_type, _id), pic_id_hash(_pidh), pic_id(_pid) {}
		uint32_t pic_id_hash{0};
		uint16_t pic_id{0};
	private:
		void Serialize(std::ostream& os) const override {
			PlayerPacket::Serialize(os);
			WritePartial(os, pic_id_hash);
		}
		void Serialize2(std::ostream& os) const override { WritePartial(os, pic_id); }
		void DeSerialize(std::istream& is) override {
			PlayerPacket::DeSerialize(is);
			pic_id_hash = ReadU32(is);
		}
		void DeSerialize2(std::istream& is) override { pic_id = ReadU16(is); }
	};

	/**
	 * Show Player Battle Animation
	 */

	class ShowPlayerBattleAnimPacket : public PlayerPacket {
	public:
		constexpr static uint8_t packet_type{ PT_SHOW_PLAYER_BATTLE_ANIM };
		ShowPlayerBattleAnimPacket() : PlayerPacket(packet_type) {}
		ShowPlayerBattleAnimPacket(uint16_t _anim_id) // C2S
			: PlayerPacket(packet_type), anim_id(_anim_id) {}
		ShowPlayerBattleAnimPacket(uint16_t _id, uint16_t _anim_id) // S2C
			: PlayerPacket(packet_type, _id), anim_id(_anim_id) {}
		uint16_t anim_id{0};
	private:
		void Serialize(std::ostream& os) const override { PlayerPacket::Serialize(os); }
		void Serialize2(std::ostream& os) const override { WritePartial(os, anim_id); }
		void DeSerialize(std::istream& is) override { PlayerPacket::DeSerialize(is); }
		void DeSerialize2(std::istream& is) override { anim_id = ReadU16(is); }
	};
}

#endif
