/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "server.h"
#include "socket.h"
#include "output_mt.h"
#include "messages.h"
#include "connection.h"
#include <thread>

#ifndef _WIN32
#  include <csignal>
#endif

#ifdef SERVER
#  include <csignal>
#  include <getopt.h>
#endif

static ServerConfig scfg;

using namespace Multiplayer;
using namespace Messages;
using namespace Chat;

class ServerConnection : public Connection {
	std::unique_ptr<Socket> socket;

	void HandleData(std::string_view data) {
		Dispatch(data);
		DispatchSystem(SystemMessage::EOD);
	}

	void HandleOpen() {
		DispatchSystem(SystemMessage::OPEN);
	}

	void HandleClose() {
		DispatchSystem(SystemMessage::CLOSE);
	}

public:
	ServerConnection(std::unique_ptr<Socket>& _socket) {
		socket = std::move(_socket);
	}

	void SetReadTimeout(uint16_t read_timeout_ms) {
		socket->SetReadTimeout(read_timeout_ms);
	}

	void Open() override {
		socket->OnInfo = [](std::string_view m) { OutputMt::Info("S: {}", m); };
		socket->OnWarning = [](std::string_view m) { OutputMt::Warning("S: {}", m); };
		socket->OnMessage = [this](auto p1) { HandleData(p1); };
		socket->OnOpen = [this]() { HandleOpen(); };
		socket->OnClose = [this]() { HandleClose(); };
		socket->Open();
	}

	void Close() override {
		socket->Close();
	}

	/**
	 * Send back to oneself
	 */

	void Send(std::string_view data) override {
		if (socket->GetWriteQueueSize() <= 80)
			socket->Send(data);
	}

	void SendAlt(std::string_view data) {
		if (socket->GetWriteQueueSize() <= 100)
			socket->Send(data);
	}
};

/**
 * Clients
 */

class ServerSideClient {
	static constexpr size_t QUEUE_MAX_BULK_SIZE = 4096;

	ServerMain* server;

	bool join_sent = false;
	bool encrypted = false;
	int id{0};
	uint32_t client_hash{0};
	ServerConnection connection;

	int room_id{0};
	uint32_t room_id_hash{0};
	uint32_t chat_crypt_key_hash{0};

	struct State {
		NamePacket name;
		MovePacket move;
		FacingPacket facing;
		SpeedPacket speed;
		SpritePacket sprite;
		RepeatingFlashPacket repeating_flash;
		TransparencyPacket transparency;
		HiddenPacket hidden;
		SystemPacket system;
		std::map<uint32_t, ShowPicturePacket> pictures;
	};
	State state;

	// Some maps won't restore their actions. Reset all here,
	//  then wait SendSelfRoomInfoAsync() to be called by clients
	void ResetState() {
		state.facing.Discard();
		state.speed.Discard();
		state.sprite.Discard();
		state.repeating_flash.Discard(); // important
		state.hidden.Discard();
		state.pictures.clear(); // important
	};

	void SendSelfRoomInfoAsync() {
		server->ForEachClient([this](const ServerSideClient& client) {
			if (client.id == id || client.room_id_hash != room_id_hash
					|| client.client_hash != client_hash)
				return;
			SendSelfAsync(JoinPacket(client.id));
			SendSelfAsync(client.state.move);
			if (client.state.facing.IsAvailable())
				SendSelfAsync(client.state.facing);
			if (client.state.speed.IsAvailable())
				SendSelfAsync(client.state.speed);
			if (client.state.name.name != "" || client.state.name.Encrypted())
				SendSelfAsync(client.state.name);
			if (client.state.sprite.IsAvailable())
				SendSelfAsync(client.state.sprite);
			if (client.state.repeating_flash.IsAvailable())
				SendSelfAsync(client.state.repeating_flash);
			if (client.state.transparency.IsAvailable())
				SendSelfAsync(client.state.transparency);
			if (client.state.hidden.IsAvailable())
				SendSelfAsync(client.state.hidden);
			if (client.state.system.name != "" || client.state.system.Encrypted())
				SendSelfAsync(client.state.system);
			for (const auto& it : client.state.pictures) {
				SendSelfAsync(it.second);
			}
		});
	}

	void InitConnection() {
		using SystemMessage = Connection::SystemMessage;

		connection.RegisterHandler<HeartbeatPacket>([this](HeartbeatPacket& p) {
			SendSelfAsync(p);
		});

		auto Leave = [this]() {
			SendLocalAsync(LeavePacket(id));
			FlushQueue();
		};

		connection.RegisterSystemHandler(SystemMessage::OPEN, [this](Connection& _) {
		});
		connection.RegisterSystemHandler(SystemMessage::CLOSE, [this, Leave](Connection& _) {
			if (join_sent) {
				Leave();
				SendGlobalChat(ChatPacket(id, 0, CV_VERBOSE, room_id, "", "*** id:"
					+ std::to_string(id) + (state.name.name == "" ? "" : " " + state.name.name) + " left the server."));
				if (encrypted)
					OutputMt::Info("S: id={} (encrypted) left the server", id);
				else
					OutputMt::Info("S: room_id={} id={} name={} left the server", room_id, id, state.name.name);
			}
			server->DeleteClient(id);
		});

		connection.RegisterHandler<ClientHelloPacket>([this](ClientHelloPacket& p) {
			if (!join_sent) {
				if (p.Encrypted()) encrypted = true;
				client_hash = p.client_hash;
				room_id = p.room_id;
				state.name.id = id;
				state.name.name = p.name;
				SendGlobalChat(ChatPacket(id, 0, CV_VERBOSE, room_id, "", "*** id:"
					+ std::to_string(id) + (state.name.name == "" ? "" : " " + state.name.name) + " joined the server."));
				if (encrypted)
					OutputMt::Info("S: id={} (encrypted) joined the server", id);
				else
					OutputMt::Info("S: room_id={} id={} name={} joined the server", room_id, id, state.name.name);
				join_sent = true;
			}
		});
		connection.RegisterHandler<RoomPacket>([this, Leave](RoomPacket& p) {
			ResetState();
			Leave();
			// Join room
			room_id = p.room_id;
			room_id_hash = p.room_id_hash;
			SendSelfAsync(p);
			SendSelfRoomInfoAsync();
			SendLocalAsync(JoinPacket(id));
			if (state.name.name != "" || state.name.Encrypted())
				SendLocalAsync(state.name);
			// ... (Waiting for the follow-up syncs of SendBasicData() from the client)
		});
		connection.RegisterHandler<NamePacket>([this](NamePacket& p) {
			p.id = id;
			state.name = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<ChatPacket>([this](ChatPacket& p) {
			p.id = id;
			p.type = 1; // 1 = chat
			VisibilityType visibility = static_cast<VisibilityType>(p.visibility);
			if (visibility == CV_LOCAL) {
				SendLocalChat(p);
				if (!p.Encrypted())
					OutputMt::Info("S: Chat: {} [LOCAL, {}]: {}", p.name, p.room_id, p.message);
			} else if (visibility == CV_GLOBAL) {
				SendGlobalChat(p);
				if (!p.Encrypted())
					OutputMt::Info("S: Chat: {} [GLOBAL, {}]: {}", p.name, p.room_id, p.message);
			} else if (visibility == CV_CRYPT) {
				// use "crypt_key_hash != 0" to distinguish whether to set or send
				if (p.crypt_key_hash != 0) { // set
					// the chat_crypt_key_hash is used for searching
					chat_crypt_key_hash = p.crypt_key_hash;
					if (p.Encrypted())
						OutputMt::Info("S: Chat: id={} [CRYPT, ?]: Update chat_crypt_key_hash: {}", id, chat_crypt_key_hash);
					else
						OutputMt::Info("S: Chat: {} [CRYPT, {}]: Update chat_crypt_key_hash: {}",
							p.name, p.room_id, chat_crypt_key_hash);
				} else { // send
					SendCryptChat(p);
				}
			}
		});
		connection.RegisterHandler<MovePacket>([this](MovePacket& p) {
			p.id = id;
			state.move = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<JumpPacket>([this](JumpPacket& p) {
			p.id = id;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<FacingPacket>([this](FacingPacket& p) {
			p.id = id;
			state.facing = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<SpeedPacket>([this](SpeedPacket& p) {
			p.id = id;
			state.speed = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<SpritePacket>([this](SpritePacket& p) {
			p.id = id;
			state.sprite = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<FlashPacket>([this](FlashPacket& p) {
			p.id = id;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<RepeatingFlashPacket>([this](RepeatingFlashPacket& p) {
			p.id = id;
			state.repeating_flash = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<RemoveRepeatingFlashPacket>([this](RemoveRepeatingFlashPacket& p) {
			p.id = id;
			state.repeating_flash.Discard();
			SendLocalAsync(p);
		});
		connection.RegisterHandler<TransparencyPacket>([this](TransparencyPacket& p) {
			p.id = id;
			state.transparency = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<HiddenPacket>([this](HiddenPacket& p) {
			p.id = id;
			state.hidden = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<SystemPacket>([this](SystemPacket& p) {
			p.id = id;
			state.system = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<SoundEffectPacket>([this](SoundEffectPacket& p) {
			p.id = id;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<ShowPicturePacket>([this](ShowPicturePacket& p) {
			p.id = id;
			if (state.pictures.size() < 200)
				state.pictures[p.pic_id_hash] = p;
			SendLocalAsync(p);
		});
		connection.RegisterHandler<MovePicturePacket>([this](MovePicturePacket& p) {
			p.id = id;
			const auto& it = state.pictures.find(p.pic_id_hash);
			if(it != state.pictures.end()) {
				PicturePacket& pic = it->second;
				if (!p.Encrypted()) pic.params = p.params;
				pic = p;
			}
			SendLocalAsync(p);
		});
		connection.RegisterHandler<ErasePicturePacket>([this](ErasePicturePacket& p) {
			p.id = id;
			state.pictures.erase(p.pic_id_hash);
			SendLocalAsync(p);
		});
		connection.RegisterHandler<ShowPlayerBattleAnimPacket>([this](ShowPlayerBattleAnimPacket& p) {
			p.id = id;
			SendLocalAsync(p);
		});

		connection.RegisterSystemHandler(SystemMessage::EOD, [this](Connection& _) {
			FlushQueue();
		});
	}

	/**
	 * Sending without queue
	 *  These will be sent back to oneself
	 */

	template<typename T>
	void SendLocalChat(const T& p) {
		server->SendTo(id, room_id_hash, CV_LOCAL, p.ToBytes(), true);
	}

	template<typename T>
	void SendGlobalChat(const T& p) {
		server->SendTo(id, 0, CV_GLOBAL, p.ToBytes(), true);
	}

	template<typename T>
	void SendCryptChat(const T& p) {
		server->SendTo(id, 0, CV_CRYPT, p.ToBytes(), true);
	}

	/**
	 * Queue sending
	 */

	std::queue<std::unique_ptr<Packet>> m_self_queue;
	std::queue<std::unique_ptr<Packet>> m_local_queue;
	std::queue<std::unique_ptr<Packet>> m_global_queue;

	template<typename T>
	void SendPacketAsync(std::queue<std::unique_ptr<Packet>>& queue,
			const T& _p) {
		auto p = new T;
		*p = _p;
		queue.emplace(p);
	}

	template<typename T>
	void SendSelfAsync(const T& p) {
		SendPacketAsync(m_self_queue, p);
	}

	template<typename T>
	void SendLocalAsync(const T& p) {
		SendPacketAsync(m_local_queue, p);
	}

	template<typename T>
	void SendGlobalAsync(const T& p) {
		SendPacketAsync(m_global_queue, p);
	}

	void FlushQueueSend(const std::string& bulk,
			const VisibilityType& visibility, bool to_self) {
		if (to_self) {
			connection.Send(bulk);
		} else {
			int to_id = 0;
			if (visibility == Chat::CV_LOCAL) {
				to_id = room_id_hash;
			}
			server->SendTo(id, to_id, visibility, bulk);
		}
	}

	void FlushQueue(std::queue<std::unique_ptr<Packet>>& queue,
			const VisibilityType& visibility, bool to_self = false) {
		std::string bulk;
		while (!queue.empty()) {
			const auto& e = queue.front();
			std::string data = e->ToBytes();
			if (bulk.size() + data.size() > QUEUE_MAX_BULK_SIZE) {
				FlushQueueSend(bulk, visibility, to_self);
				bulk.clear();
			}
			bulk += data;
			queue.pop();
		}
		if (!bulk.empty()) {
			FlushQueueSend(bulk, visibility, to_self);
		}
	}

	void FlushQueue() {
		FlushQueue(m_global_queue, CV_GLOBAL);
		FlushQueue(m_local_queue, CV_LOCAL);
		FlushQueue(m_self_queue, CV_NONE, true);
	}

public:
	ServerSideClient(ServerMain* _server, int _id, std::unique_ptr<Socket> _socket)
			: server(_server), id(_id), connection(ServerConnection(_socket)) {
		InitConnection();
	}

	void Open() {
		connection.SetReadTimeout(scfg.no_heartbeats ? 0 : 6000);
		connection.Open();
	}

	void Close() {
		connection.Close();
	}

	void Send(std::string_view data, bool alt) {
		if (alt)
			connection.SendAlt(data);
		else
			connection.Send(data);
	}

	void SendAlt(std::string_view data) {
		connection.SendAlt(data);
	}

	int GetId() const {
		return id;
	}

	int GetClientHash() const {
		return client_hash;
	}

	int GetRoomIdHash() const {
		return room_id_hash;
	}

	int GetChatCryptKeyHash() const {
		return chat_crypt_key_hash;
	}
};

/**
 * Server
 */

struct ServerMain::DataToSend {
	int from_id;
	int to_id;
	VisibilityType visibility;
	std::string data;
	bool return_flag;
};

void ServerMain::ForEachClient(const std::function<void(ServerSideClient&)>& callback) {
	std::lock_guard lock(m_mutex);
	if (!running) return;
	for (const auto& it : clients) {
		callback(*it.second);
	}
}

void ServerMain::DeleteClient(const int id) {
	std::lock_guard lock(m_mutex);
	clients.erase(id);
}

void ServerMain::SendTo(const int from_id, const int to_id,
		const VisibilityType visibility, std::string_view data,
		const bool return_flag) {
	std::lock_guard lock(m_mutex);
	if (!running) return;
	auto data_to_send = new DataToSend{ from_id, to_id, visibility, std::string(data), return_flag };
	m_data_to_send_queue.emplace(data_to_send);
	m_data_to_send_queue_cv.notify_one();
}

void ServerMain::Start(bool wait_thread) {
	if (running) return;
	running = true;

	std::thread([this]() {
		while (running) {
			std::unique_lock<std::mutex> lock(m_mutex);
			m_data_to_send_queue_cv.wait(lock, [this] {
					return !m_data_to_send_queue.empty(); });
			auto& data_to_send = m_data_to_send_queue.front();
			// stop the thread
			if (data_to_send->from_id == 0 &&
					data_to_send->visibility == Chat::CV_NONE) {
				m_data_to_send_queue.pop();
				break;
			}
			// check if the client is online
			ServerSideClient* from_client = nullptr;
			const auto& from_client_it = clients.find(data_to_send->from_id);
			if (from_client_it != clients.end()) {
				from_client = from_client_it->second.get();
			}
			// send to global, local and crypt
			if (data_to_send->visibility == Chat::CV_LOCAL ||
				data_to_send->visibility == Chat::CV_CRYPT ||
					data_to_send->visibility == Chat::CV_GLOBAL) {
				// enter on every client
				for (const auto& it : clients) {
					auto& to_client = it.second;
					// exclude self
					if (!data_to_send->return_flag &&
							data_to_send->from_id == to_client->GetId())
						continue;
					if (from_client && from_client->GetClientHash() != to_client->GetClientHash()) continue;
					bool send_alt = data_to_send->return_flag;
					// send to local
					if (data_to_send->visibility == Chat::CV_LOCAL &&
							data_to_send->to_id == to_client->GetRoomIdHash()) {
						to_client->Send(data_to_send->data, send_alt);
					// send to crypt
					} else if (data_to_send->visibility == Chat::CV_CRYPT &&
							from_client && from_client->GetChatCryptKeyHash() == to_client->GetChatCryptKeyHash()) {
						to_client->Send(data_to_send->data, send_alt);
					// send to global
					} else if (data_to_send->visibility == Chat::CV_GLOBAL) {
						to_client->Send(data_to_send->data, send_alt);
					}
				}
			}
			m_data_to_send_queue.pop();
		}
	}).detach();

	auto CreateServerSideClient = [this](std::unique_ptr<Socket> socket) {
		std::lock_guard lock(m_mutex);
		if (clients.size() >= scfg.max_users) {
			socket->OnInfo = [](std::string_view m) {
				OutputMt::Info("S: {} (Too many users)", m);
			};
			socket->Send("\uFFFD1");
			socket->Close();
			socket->MoveSocketPtr(socket);
		} else {
			auto& client = clients[client_id];
			client.reset(new ServerSideClient(this, client_id++, std::move(socket)));
			client->Open();
		}
	};

	if (!scfg.bind_address_2.empty()) {
		std::string addr_host_2;
		uint16_t addr_port_2{ 6500 };
		Connection::ParseAddress(scfg.bind_address_2, addr_host_2, addr_port_2);
		server_listener_2.reset(new ServerListener(addr_host_2, addr_port_2));
		server_listener_2->OnInfo = [](std::string_view m) { OutputMt::Info("S: {}", m); };
		server_listener_2->OnWarning = [](std::string_view m) { OutputMt::Warning("S: {}", m); };
		server_listener_2->OnConnection = CreateServerSideClient;
		server_listener_2->Start();
	}

	std::string addr_host;
	uint16_t addr_port{ 6500 };
	Connection::ParseAddress(scfg.bind_address, addr_host, addr_port);
	server_listener.reset(new ServerListener(addr_host, addr_port));
	server_listener->OnInfo = [](std::string_view m) { OutputMt::Info("S: {}", m); };
	server_listener->OnWarning = [](std::string_view m) { OutputMt::Warning("S: {}", m); };
	server_listener->OnConnection = CreateServerSideClient;
	server_listener->Start(wait_thread);
}

void ServerMain::Stop() {
	std::lock_guard lock(m_mutex);
	if (!running) return;
	running = false;
	Output::Debug("Server: Stopping");
	for (const auto& it : clients) {
		it.second->SendAlt("\uFFFD0");
		// the client will be removed upon SystemMessage::CLOSE
		it.second->Close();
	}
	if (server_listener_2)
		server_listener_2->Stop();
	server_listener->Stop();
	// stop sending loop
	m_data_to_send_queue.emplace(new DataToSend{ 0, 0, Chat::CV_NONE, "" });
	m_data_to_send_queue_cv.notify_one();
	Output::Info("S: Stopped");
}

#ifndef SERVER
void ServerMain::SetConfig(const Game_ConfigMultiplayer& cfg) {
	scfg.no_heartbeats = cfg.no_heartbeats.Get();
	scfg.bind_address = cfg.server_bind_address.Get();
	scfg.bind_address_2 = cfg.server_bind_address_2.Get();
	scfg.max_users = cfg.server_max_users.Get();
}
#endif

static ServerMain _instance;

ServerMain& Server() {
#ifndef _WIN32
	// Prevent SIGPIPE caused by remote connection close
	// To disable SIGPIPE in GDB: (gdb) handle SIGPIPE nostop
	std::signal(SIGPIPE, SIG_IGN);
#endif
	return _instance;
}

#ifdef SERVER
int main(int argc, char *argv[])
{
	// Character followed by a colon requires an argument
	const char* short_opts = "na:A:U:";

	const option long_opts[] = {
		{"no-heartbeats", no_argument, nullptr, 'n'},
		{"bind-address", required_argument, nullptr, 'a'},
		{"bind-address-2", required_argument, nullptr, 'A'},
		{"max-users", required_argument, nullptr, 'U'},
		{nullptr, no_argument, nullptr, 0}
	};

	while (true) {
		const auto opt = getopt_long(argc, argv, short_opts, long_opts, nullptr);
		if (opt == 'n')
			scfg.no_heartbeats = true;
		else if (opt == 'a')
			scfg.bind_address = optarg;
		else if (opt == 'A')
			scfg.bind_address_2 = optarg;
		else if (opt == 'U')
			scfg.max_users = atoi(optarg);
		else
			break;
	}

	auto signal_handler = [](int signal) {
		Server().Stop();
		Output::Debug("Server: signal={}", signal);
	};
	std::signal(SIGINT, signal_handler);
	std::signal(SIGTERM, signal_handler);

	Server().Start(true);

	return EXIT_SUCCESS;
}
#endif
