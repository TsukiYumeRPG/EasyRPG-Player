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

#ifndef EP_MULTIPLAYER_CHATUI_H
#define EP_MULTIPLAYER_CHATUI_H

#include <cstdint>
#include <string>

struct ChatUiTextConfig {
	uint8_t color_status_connection = 2;
	uint8_t color_status_room = 1;
	uint8_t color_log_divider = 1;
	uint8_t color_log_name = 0;
	uint8_t color_log_visibility = 2;
	uint8_t color_log_room = 1;
	uint8_t color_log_time = 0;
	uint8_t color_log_message = -1;
	uint8_t color_log_truncatechar = 1;
	uint8_t color_typebox = 0;
	uint8_t color_print_message = 0;
	uint8_t color_print_label = 1;
	uint8_t color_print_label_message = 0;
};

class ChatUi {
public:
	static ChatUi& Instance();

	void Refresh(); // initializes chat or refreshes its theme
	void Update(); // called once per logical frame
	void OnResolutionChange();
	void SetFocus(bool focused);

	void GotMessage(int visibility, int room_id, std::string name,
			std::string message, std::string sys_name);
	void GotSystemMessage(std::string msg, int visibility = 0);

	void SetTextConfig(ChatUiTextConfig tcfg);
	void SetStatusConnection(bool connected, bool connecting = false);
	void SetStatusRoom(unsigned int room_id);
	void SetStatusProgress(unsigned int percent, std::string text);

	bool IsCheating() const;
};

inline ChatUi& CUI() { return ChatUi::Instance(); }

#endif
