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

#include "chatui.h"
#include <memory>
#include <vector>
#include "../window_base.h"
#include "../scene.h"
#include "../scene_debug.h"
#include "../bitmap.h"
#include "../drawable_mgr.h"
#include "../font.h"
#include "../cache.h"
#include "../input.h"
#include "../utils.h"
#include "../player.h"
#include "../compiler.h"
#include "../baseui.h"
#include "../graphics.h"
#include "../statustext_overlay.h"
#include "../debugtext_overlay.h"
#include "../game_switches.h"
#include "../game_variables.h"
#include "../game_map.h"
#include "../version.h"
#include "output_mt.h"
#include "game_multiplayer.h"
#include "util/strfnd.h"
#include "util/crypto.h"
#include "chat.h"

#ifndef EMSCRIPTEN
#  include "server.h"
#endif

namespace {

const int CHATLOG_MAX_MINIMIZED_MESSAGES = 3;
const int CHATLOG_MAX_TOTAL_MESSAGES = 1000;
const int CHATLOG_MAX_MESSAGES = 500;
const int CHATLOG_MAX_CHARS_INPUT = 200;
const int CHATLOG_LINE_BREAK_MULTIPLIER = 10;

using VisibilityType = Chat::VisibilityType;

ChatUiTextConfig tcfg;

/**
 * OnlineStatus
 */

class DrawableOnlineStatus : public Drawable {
	Rect bounds;

	// design parameters
	const unsigned int padding_horz = 4; // padding between box edges and content (left)
	const unsigned int padding_vert = 6; // padding between box edges and content (top)
	// constructor start

	BitmapRef conn_status;
	BitmapRef room_status;

	bool m_status = false;
	bool m_connecting = false;
	unsigned int m_room_id = 0;
	unsigned int m_progress_percent = 100;

public:
	DrawableOnlineStatus(int x, int y, int w, int h)
		: Drawable(Priority::Priority_Maximum, Drawable::Flags::Global),
		bounds(x, y, w, h)
	{
		DrawableMgr::Register(this);

		// initialize
		SetConnectionStatus(false);
		SetRoomStatus(0);
	}

	void SetX(unsigned int x) {
		bounds.x = x;
	}

	void SetY(unsigned int y) {
		bounds.y = y;
	}

	void Draw(Bitmap& dst) {
		dst.Blit(bounds.x + padding_horz, bounds.y + padding_vert,
			*conn_status, conn_status->GetRect(), Opacity::Opaque());
		auto r_rect = room_status->GetRect();
		dst.Blit(bounds.x + bounds.width - padding_horz - r_rect.width, bounds.y + padding_vert,
			*room_status, r_rect, Opacity::Opaque());
	};

	void RefreshTheme() {
		UpdateConnectionStatus();
		UpdateRoomStatus();
	}

	void SetConnectionStatus(bool status, bool connecting = false) {
		m_status = status;
		m_connecting = connecting;

		std::string conn_label = "";
		if (connecting)
			conn_label = "Connecting";
		else
			conn_label = status ? "Connected" : "Disconnected";

		auto c_rect = Text::GetSize(*Font::Default(), conn_label);
		conn_status = Bitmap::Create(c_rect.width + 1, c_rect.height + 1);
		Text::Draw(*conn_status, 0, 0, *Font::Default(), *Cache::SystemOrBlack(),
			tcfg.color_status_connection, conn_label);
	}

	void UpdateConnectionStatus() {
		SetConnectionStatus(m_status, m_connecting);
	}

	void SetRoomStatus(unsigned int room_id) {
		m_room_id = room_id;

		std::string room_label = "";

		if (m_progress_percent < 100) {
			room_label = std::to_string(m_progress_percent) + "% #" + std::to_string(room_id);
		} else {
			room_label = "Room #" + std::to_string(room_id);
		}

		auto r_rect = Text::GetSize(*Font::Default(), room_label);
		room_status = Bitmap::Create(r_rect.width + 1, r_rect.height + 1);
		Text::Draw(*room_status, 0, 0, *Font::Default(), *Cache::SystemOrBlack(), tcfg.color_status_room, room_label);
	}

	void UpdateRoomStatus() {
		SetRoomStatus(m_room_id);
	}

	void SetProgressStatus(unsigned int percent) {
		if (m_progress_percent != percent) {
			m_progress_percent = percent;
			UpdateRoomStatus();
		}
	}
};

/**
 * ChatLog
 */

using ChatLogText = std::vector<std::pair<std::string, int8_t>>;

struct ChatLogMessageData {
	ChatLogText text;
	VisibilityType visibility;
	std::string sys_name;
	bool break_word;
	bool remove_message = false; // Used for simulating line break placeholder
	ChatLogMessageData(ChatLogText&& t, VisibilityType v, std::string&& sys, bool bw) {
		text = std::move(t), visibility = v, sys_name = std::move(sys), break_word = bw;
	}
};

class DrawableChatLog : public Drawable {
	struct DrawableMessage {
		std::unique_ptr<ChatLogMessageData> message_data;
		BitmapRef render_graphic;
		bool dirty; // need to redraw? (for when UI skin changes)
		BitmapRef selection_graphic;
		int caret_index_tail = 0;
		int caret_index_head = 0;
		std::vector<Rect> caret_char_dims;
		DrawableMessage(std::unique_ptr<ChatLogMessageData>& msg) {
			message_data = std::move(msg);
			render_graphic = nullptr;
			dirty = true;
			selection_graphic = nullptr;
		}
	};

	Rect bounds;

	// design parameters
	const unsigned int message_padding = 1;
	const unsigned int message_padding_overlay = 2;
	const unsigned int scroll_frame = 8; // width of scroll bar's visual frame (on right side)
	const unsigned int scroll_bleed = 2; // how much to stretch right edge of scroll box offscreen (so only left frame shows)
	const unsigned int caret_left_kerning = 6; // adjust spacing for better readability near the caret
	const std::string caret_char = "｜"; // caret (type cursor) graphic
	// constructor start

	Window_Base scroll_box; // box used as rendered design for a scrollbar
	int z_index = 0;

	bool overlay = false; // is it used for overlay? it has a translucent background
	bool overlay_minimized = false; // timeout removal, single line, and limit log
	float removal_counter = 0;

	int message_index_tail = 0, message_index_head = 0;
	std::vector<DrawableMessage> d_messages;
	std::unordered_map<VisibilityType, int> messages_count;
	unsigned int content_height = 0;
	int scroll_position = 0;
	unsigned short visibility_flags = Chat::CV_LOCAL | Chat::CV_GLOBAL | Chat::CV_CRYPT;
	bool editable = false;

	BitmapRef caret;
	bool caret_shown = false;
	bool caret_movable = true;
	bool caret_blink_shown = true;
	bool caret_move = false;
	bool caret_follow_scroll = false;
	float caret_blink_counter = 0;

	BitmapRef default_theme;
	BitmapRef current_theme;

	void BuildMessageGraphic(DrawableMessage& d_msg) {
		struct Glyph {
			Utils::TextRet data;
			Rect dims;
			int8_t color;
		};
		using GlyphLine = std::vector<Glyph>;
		auto ExtractGlyphs = [](StringView str, int8_t color, GlyphLine& line, unsigned int& width) {
			const auto* iter = str.data();
			const auto* end = str.data() + str.size();
			while (iter != end) {
				auto resp = Utils::TextNext(iter, end, 0);
				iter = resp.next;

				Rect ch_rect;
				if (resp.is_exfont) {
					ch_rect = Text::GetSize(*Font::exfont, " ");
				} else {
					if (resp.ch == u'\uFF00')
						// Avoid using GetSize to prevent "glyph not found"
						ch_rect = Rect(0, 0, 0, 12);
					else
						ch_rect = Font::Default()->GetSize(resp.ch);
				}

				line.push_back({ resp, ch_rect, color });
				width += ch_rect.width;
			}
		};
		auto FindFirstGlyphOf = [](GlyphLine& line, char32_t ch) -> int {
			unsigned int n_glyphs = line.size();
			for (int i = 0; i < n_glyphs; ++i) {
				if (line[i].data.ch == ch) {
					return i;
				}
			}
			return -1;
		};
		auto FindLastGlyphOf = [](GlyphLine& line, char32_t ch) -> int {
			unsigned int n_glyphs = line.size();
			for (int i = n_glyphs - 1; i >= 0; --i) {
				if (line[i].data.ch == ch) {
					return i;
				}
			}
			return -1;
		};
		auto MoveGlyphsToNext = [](GlyphLine& curr, GlyphLine& next,
				unsigned int& curr_width, unsigned int& next_width, unsigned int amount) {
			/** pop `curr` last glyph, insert `next` in reverse order (begin()) */
			for (int i = 0; i < amount; ++i) {
				auto& glyph = curr.back();
				unsigned int delta_width = glyph.dims.width;
				next.insert(next.begin(), glyph);
				next_width += delta_width;
				curr.pop_back();
				curr_width -= delta_width;
			}
		};
		auto GetLineHeight = [](GlyphLine& line) -> unsigned int {
			unsigned int height = 0;
			unsigned int n_glyphs = line.size();
			for (int i = 0; i < n_glyphs; ++i) {
				height = std::max<unsigned int>(height, line[i].dims.height);
			}
			return height;
		};

		unsigned int padding = overlay ? message_padding_overlay : message_padding;
		unsigned int padding_dims = padding * 2;

		// manual text wrapping
		const unsigned int max_width = bounds.width - scroll_frame - padding_dims;

		// individual lines saved so far, along with their y offset
		std::vector<std::pair<GlyphLine, unsigned int>> lines;
		unsigned int total_width = 0; // maximum width between all lines
		unsigned int total_height = 0; // accumulated height from all lines

		GlyphLine glyphs_current; // list of glyphs and their dimensions on current line
		GlyphLine glyphs_next; // stores glyphs moved down to line below
		unsigned int width_current = 0; // total width of glyphs on current line
		unsigned int width_next = 0; // total width of glyphs on next line

		BitmapRef graphic;
		if (d_msg.message_data->sys_name == "") {
			graphic = current_theme;
		} else {
			graphic = Cache::System(d_msg.message_data->sys_name);
		}

		// break down whole message string into glyphs for processing.
		// glyph lookup is performed only at this stage, and their dimensions are saved for subsequent line width recalculations.
		for (const auto& t : d_msg.message_data->text) ExtractGlyphs(t.first, t.second, glyphs_current, width_current);

		bool break_word = d_msg.message_data->break_word;

		// break down message into fitting lines
		do {
			while (width_current > max_width) {
				// as long as current line exceeds maximum width,
				// move one word from this line down to the next one
				int last_space = FindLastGlyphOf(glyphs_current, ' ');
				if (break_word && last_space != -1 && last_space < glyphs_current.size() - 1) {
					// there is a word that can be moved down
					MoveGlyphsToNext(glyphs_current, glyphs_next,
						width_current, width_next, glyphs_current.size() - last_space - 1);
				} else {
					// there is not a whole word that can be moved down, so move individual characters.
					// this case happens when last character in current line is a space,
					// or when there are no spaces in the current line
					MoveGlyphsToNext(glyphs_current, glyphs_next, width_current, width_next, 1);
				}
			}
			// once line fits, check for line breaks
			int line_break = FindFirstGlyphOf(glyphs_current, '\n');
			if (line_break != -1) {
				MoveGlyphsToNext(glyphs_current, glyphs_next,
					width_current, width_next, glyphs_current.size() - line_break - 1);
			}
			// a special character used to align text to the right
			int remaining_width_char = FindLastGlyphOf(glyphs_current, u'\uFF00');
			if (remaining_width_char != -1) {
				glyphs_current[remaining_width_char].dims.width = max_width - width_current;
				width_current = max_width;
			}
			// save current line
			lines.push_back(std::make_pair(glyphs_current, total_height));
			total_width = std::max<unsigned int>(total_width, width_current + padding_dims);
			total_height += GetLineHeight(glyphs_current) + padding_dims;
			// repeat this work on the exceeding portion moved down to the line below
			glyphs_current = glyphs_next;
			glyphs_next.clear();
			width_current = width_next;
			width_next = 0;
			if (overlay && overlay_minimized && glyphs_current.size() > 0) {
				// use '>' as a truncation character
				auto& last_glyph = lines.front().first.back();
				last_glyph.data.ch = '>';
				last_glyph.color = tcfg.color_log_truncatechar;
				break;
			}
		} while (glyphs_current.size() > 0);

		// show caret only when blank
		if (d_msg.message_data->text.size() == 1 && d_msg.message_data->text.front().first.empty())
			total_height = caret->GetRect().height + padding_dims;

		if (d_msg.render_graphic) {
			content_height += total_height - d_msg.render_graphic->GetRect().height;
			OnContenHeightChanged();
		}

		// once all lines have been saved
		// render them into a bitmap
		BitmapRef text_img = Bitmap::Create(total_width + (overlay ? 0 : caret->GetRect().width), total_height);
		// add background
		if (overlay && total_width > padding_dims) text_img->Fill(Color(0, 0, 0, 102));
		d_msg.caret_char_dims.clear();
		int n_lines = lines.size(), glyph_acc_x = padding;
		for (int i = 0; i < n_lines; ++i) {
			auto& line = lines[i];
			int glyph_y = padding + line.second;
			glyph_acc_x = padding;
			for (int j = 0; j < line.first.size(); ++j) {
				auto& glyph = line.first[j];
				auto ret = glyph.data;
				int glyph_x = glyph_acc_x;
				if (EP_UNLIKELY(!ret)) continue;
				if (ret.ch == u'\uFF00') {
					text_img->ClearRect(Rect(glyph_x, glyph_y - padding,
						glyph.dims.width + padding_dims, glyph.dims.height + padding_dims));
					glyph_acc_x += glyph.dims.width + padding_dims;
					glyph.dims.width = 0; // do not show selection
				} else {
					if (glyph.color < 0) {
						glyph.color = 0;
						graphic = default_theme;
					}
					glyph_acc_x += Text::Draw(*text_img, glyph_x, glyph_y,
						*Font::Default(), *graphic, glyph.color, ret.ch, ret.is_exfont).x;
				}
				glyph.dims.x = glyph_x, glyph.dims.y = glyph_y;
				d_msg.caret_char_dims.push_back(glyph.dims);
			}
		}
		Rect caret_dims = caret->GetRect();
		caret_dims.x = glyph_acc_x;
		caret_dims.y = d_msg.caret_char_dims.empty() ? padding : d_msg.caret_char_dims.back().y;
		d_msg.caret_char_dims.push_back(caret_dims);

		d_msg.render_graphic = text_img;
		d_msg.dirty = false;
	}

	void BuildCaretGraphic() {
		auto c_rect = Text::GetSize(*Font::Default(), caret_char);
		caret = Bitmap::Create(c_rect.width - caret_left_kerning, c_rect.height);
		Text::Draw(*caret, -caret_left_kerning, 0, *Font::Default(), *current_theme, 0, caret_char);
	}

	void BuildSelectionGraphic(DrawableMessage& d_msg) {
		bool created = false;

		// Draw vertical selections
		if (!overlay && message_index_tail != message_index_head) {
			const unsigned int msg_start = std::min<unsigned int>(message_index_tail, message_index_head);
			const unsigned int msg_end = std::max<unsigned int>(message_index_tail, message_index_head);
			for (int i = msg_start; i < msg_end; ++i) {
				DrawableMessage& d_i_msg = d_messages[i];
				auto rect = d_i_msg.render_graphic->GetRect();
				// Avoid repeated bitmap creation to prevent clearing horizontal selections
				if (!d_i_msg.selection_graphic || &d_msg == &d_i_msg) {
					// Clear the bitmap, redraw vertical selections, then horizontal selections
					d_i_msg.selection_graphic = Bitmap::Create(rect.width, rect.height);
				}
				Rect char_rect = d_i_msg.caret_char_dims.back();
				d_i_msg.selection_graphic->ClearRect(char_rect);
				d_i_msg.selection_graphic->FillRect(char_rect, Color(255, 255, 255, 100));
				if (&d_msg == &d_i_msg)
					created = true;
			}
		}

		// Draw horizontal selections
		if(d_msg.caret_index_tail != d_msg.caret_index_head) {
			const unsigned int caret_start = std::min<unsigned int>(d_msg.caret_index_tail, d_msg.caret_index_head);
			const unsigned int caret_end = std::max<unsigned int>(d_msg.caret_index_tail, d_msg.caret_index_head);
			if (!created) {
				auto rect = d_msg.render_graphic->GetRect();
				d_msg.selection_graphic = Bitmap::Create(rect.width, rect.height);
			}
			for (int i = caret_start; i < caret_end; ++i) {
				Rect char_rect = d_msg.caret_char_dims[i];
				d_msg.selection_graphic->FillRect(char_rect, Color(255, 255, 255, 100));
			}
			created = true;
		}

		if (!created && d_msg.selection_graphic)
			d_msg.selection_graphic = nullptr;
	}

	void RefreshMessages() {
		for (int i = 0; i < d_messages.size(); ++i) d_messages[i].dirty = true;
	}

	void AddLogEntry(const ChatLogMessageData* p, std::unique_ptr<ChatLogMessageData>& msg) {
		DrawableMessage d_msg = DrawableMessage(msg);
		BuildMessageGraphic(d_msg);
		if (MessageVisible(d_msg, visibility_flags)) {
			content_height += d_msg.render_graphic->GetRect().height;
			OnContenHeightChanged();
			RefreshScroll();
		}
		if (!editable) {
			int limit = overlay_minimized ? CHATLOG_MAX_MINIMIZED_MESSAGES : CHATLOG_MAX_TOTAL_MESSAGES;
			if (d_messages.size() >= limit)
				RemoveLogEntry();
			VisibilityType v = d_msg.message_data->visibility;
			if (v > 0 && v < Chat::CV_MAX) {
				if (messages_count[v] >= CHATLOG_MAX_MESSAGES) {
					RemoveLogEntry(nullptr, v);
				} else {
					messages_count[v] += 1;
				}
			}
		}
		if (p != nullptr) {
			for (int i = 0; i < d_messages.size(); ++i) {
				if (p == d_messages[i].message_data.get()) {
					d_messages.insert(d_messages.begin() + i, std::move(d_msg));
					break;
				}
			}
		} else {
			d_messages.push_back(std::move(d_msg));
		}
		CaretFollowScroll();
	}

	void RemoveLogEntry(const ChatLogMessageData* p = nullptr, VisibilityType v = Chat::CV_NONE) {
		for (int i = 0; i < d_messages.size(); ++i) {
			DrawableMessage& d_msg = d_messages[i];
			if (p == d_msg.message_data.get() || (p == nullptr && v == Chat::CV_NONE)
					|| d_msg.message_data->visibility == v) {
				if (MessageVisible(d_msg, visibility_flags)) {
					content_height -= d_msg.render_graphic->GetRect().height;
					OnContenHeightChanged();
					RefreshScroll();
				}
				d_messages.erase(d_messages.begin() + i);
				break;
			}
		}
		CaretFollowScroll();
	}

	void ClampScrollPosition() {
		// maximum value for scroll_position (= amount of height escaping from the top)
		const int max_scroll = std::max<int>(0, content_height - bounds.height);
		scroll_position = std::clamp(scroll_position, 0, max_scroll);
	}

	void UpdateScrollBar() {
		if (content_height <= bounds.height) {
			// hide scrollbar if content isn't large enough for scroll
			scroll_box.SetX(bounds.x + bounds.width + scroll_frame);
			return;
		}
		scroll_box.SetX(bounds.x + bounds.width - scroll_frame); // show scrollbar
		// position scrollbar
		const float ratio = bounds.height / float(content_height);
		const unsigned int bar_height = bounds.height * ratio;
		// clamp the scroll_box minimum height
		const unsigned int bar_height_safe = std::max<unsigned int>(bar_height, 16);
		const unsigned int bar_y = scroll_position * ratio;
		const unsigned int bar_offset_safe = (bar_height_safe - bar_height)
			* (1.0f - float(bar_y) / bounds.height);
		scroll_box.SetHeight(bar_height_safe);
		scroll_box.SetY(bounds.y + bounds.height - bar_y - bar_height - bar_offset_safe);
	}

	void RefreshScroll() {
		ClampScrollPosition();
		UpdateScrollBar();
	}

	bool MessageVisible(const DrawableMessage& d_msg, unsigned short v) {
		return (d_msg.message_data->visibility & v) > 0;
	}

	int GetLength() {
		int count = 0;
		for (const auto& d_msg : d_messages) {
			for (const auto& t : d_msg.message_data->text)
				count += Utils::DecodeUTF32(t.first).size();
			count += CHATLOG_LINE_BREAK_MULTIPLIER;
		}
		return count;
	}

	void CaretFollowScroll() {
		if (!caret_shown) caret_follow_scroll = true;
	}

public:
	std::function<void()> OnContenHeightChanged = []() {};
	std::function<void(Rect)> OnCaretMoved = [](Rect) {};

	DrawableChatLog(int x, int y, int w, int h, int z_index)
		: Drawable(Priority::Priority_Maximum + z_index, Drawable::Flags::Global),
		bounds(x, y, w, h), z_index(z_index),
		scroll_box(0, 0, scroll_frame + scroll_bleed, 0, Drawable::Flags::Global)
	{
		DrawableMgr::Register(this);

		scroll_box.SetZ(Priority::Priority_Maximum + z_index - 1);
		scroll_box.SetVisible(false);

		current_theme = Cache::SystemOrBlack();
		default_theme = current_theme;

		BuildCaretGraphic();
	}

	void Draw(Bitmap& dst) {
		// - Draw message bitmaps to dst and adjust its position on dst using scroll y
		// - As the message bitmap moves off-screen, let its height approach 0, and completely off-screen messages are ignored
		// - scroll range: minimum (newest), maximum (oldest). i range: minimum (oldest), maximum (newest)
		// y offset to draw next message, from bottom of log panel
		int next_height = -scroll_position;
		int min_i = -1, max_i = -1;
		unsigned int num_d_msgs = d_messages.size();
		for (int i = num_d_msgs - 1; i >= 0; --i) {
			DrawableMessage& d_msg = d_messages[i];
			// skip drawing hidden messages
			if (!MessageVisible(d_msg, visibility_flags))
				continue;
			// rebuild message graphic if needed
			if (d_msg.dirty)
				BuildMessageGraphic(d_msg);
			auto rect = d_msg.render_graphic->GetRect();
			// accumulate y offset
			next_height += rect.height;
			// skip drawing offscreen messages, but still accumulate y offset (bottom offscreen)
			if (next_height <= 0) {
				// Scroll down to follow the caret (newest content direction)
				if (caret_shown && caret_move && message_index_head > i - 1) {
					Scroll(-d_messages[message_index_head - 1].render_graphic->GetRect().height);
				}
				continue;
			}
			if (max_i == -1) max_i = i - 1;
			// cutoff message graphic so text does not bleed out of bounds
			// top_offscreen: the part that exceeds the top of the screen
			const unsigned int top_offscreen = std::max<int>(next_height - bounds.height, 0);
			Rect cutoff_rect = Rect(rect.x, rect.y + top_offscreen,
				rect.width, std::min<unsigned int>(rect.height, next_height) - top_offscreen);
			// draw contents
			unsigned int base_x = bounds.x;
			unsigned int base_y = bounds.y + bounds.height - next_height + top_offscreen;
			dst.Blit(base_x, base_y, *(d_msg.render_graphic), cutoff_rect, Opacity::Opaque());
			// draw caret
			if (caret_shown && i == message_index_head) {
				++caret_blink_counter;
				if (Game_Clock::GetFPS() > 0.0f && caret_blink_counter > Game_Clock::GetFPS() * 0.5f) {
					caret_blink_counter = 0.0f;
					caret_blink_shown = !caret_blink_shown;
				}
				const auto& caret_dims = d_msg.caret_char_dims[d_msg.caret_index_head];
				int caret_y = base_y + caret_dims.y - top_offscreen; // it can't use cutoff_rect，minus top_offscreen
				Rect caret_rect = caret->GetRect();
				Rect caret_cuteoff_rect = caret_rect;
				caret_cuteoff_rect.y = caret_cuteoff_rect.y + std::max<int>(bounds.y - caret_y, 0);
				caret_cuteoff_rect.height = std::min<int>(
					caret_cuteoff_rect.height, bounds.height - (caret_y - bounds.y));
				if (caret_blink_shown) {
					dst.Blit(base_x + caret_dims.x, caret_y + caret_cuteoff_rect.y,
						*caret, caret_cuteoff_rect, Opacity::Opaque());
				}
				if (caret_move) {
					// Scroll the screen when it's off the top
					if (top_offscreen > 0) Scroll(top_offscreen);
					// Scroll the screen when it's off the bottom
					if (caret_y - bounds.y + caret_rect.height > bounds.height)
						Scroll(-(caret_rect.height - caret_cuteoff_rect.height));
					OnCaretMoved(Rect(base_x + caret_dims.x - bounds.x, caret_y - bounds.y,
						caret_dims.width, caret_dims.height));
					caret_move = false;
				}
			}
			// draw selection
			if (d_msg.selection_graphic)
				dst.Blit(base_x, base_y, *(d_msg.selection_graphic), cutoff_rect, Opacity::Opaque());
			// stop drawing offscreen messages (top offscreen)
			if (next_height > bounds.height) {
				if (min_i == -1) min_i = i + 1;
				// Scroll up to follow the caret (oldest content direction)
				if (caret_shown && caret_move && message_index_head < i) {
					Scroll(d_messages[message_index_head].render_graphic->GetRect().height);
				}
				break;
			}
		}

		if (caret_follow_scroll && message_index_tail == message_index_head) {
			if (message_index_head > max_i || message_index_head < min_i) {
				for (int i = max_i; i > min_i + 1 ; --i) {
					if (MessageVisible(d_messages[i], visibility_flags)) {
						message_index_tail = message_index_head = i;
						break;
					}
				}
			}
			caret_follow_scroll = false;
		}

		// automatically remove messages
		if (overlay && overlay_minimized && d_messages.size() > 0) {
			++removal_counter;
			// the delay is 3 seconds
			if (Game_Clock::GetFPS() > 0.0f && removal_counter > Game_Clock::GetFPS() * 3.0f) {
				removal_counter = 0.0f;
				RemoveLogEntry();
			}
		}
	};

	void SetOverlayMode(bool enabled, bool minimized = false) {
		overlay = enabled;
		overlay_minimized = minimized;

		RefreshMessages();
	}

	void SetMode(bool show_caret, bool enable_editable) {
		caret_shown = show_caret;
		editable = enable_editable;

		if (editable) {
			if (d_messages.empty()) {
				AddLogEntry(std::make_unique<ChatLogMessageData>(ChatLogText({{"", 0}}),
					Chat::CV_LOCAL, "", true));
			}
		}
	}

	void SetX(unsigned int x) {
		bounds.x = x;
	}

	void SetY(unsigned int y) {
		bounds.y = y;
		RefreshScroll();
	}

	void SetHeight(unsigned int h) {
		bounds.height = h;
		RefreshScroll();
	}

	unsigned int GetContentHeight() {
		return content_height;
	}

	unsigned int GetVisibleMessageCount() {
		int count = 0;
		for (const auto& d_msg : d_messages)
			if (MessageVisible(d_msg, visibility_flags)) ++count;
		return count;
	}

	void RefreshTheme() {
		auto new_theme = Cache::SystemOrBlack();

		// do nothing if theme hasn't changed
		if (new_theme == current_theme)
			return;

		current_theme = new_theme;
		scroll_box.SetWindowskin(current_theme);

		RefreshMessages(); // all messages now need to be redrawn with different UI skin
		BuildCaretGraphic();
	}

	void AddLogEntry(std::unique_ptr<ChatLogMessageData>&& msg) {
		AddLogEntry(nullptr, msg);
	}

	void Scroll(int delta) {
		scroll_position += delta;
		RefreshScroll();
		CaretFollowScroll();
	}

	void ShowScrollBar(bool v) {
		scroll_box.SetVisible(v);
	}

	unsigned short GetVisibilityFlags() {
		return visibility_flags;
	}

	void ToggleVisibilityFlag(VisibilityType v) {
		// Expands/collapses messages in-place, so you don't get lost if you've scrolled far up.
		//
		// Finds the bottommost (before the change) message that is visible both before and after changing visibility flags,
		//  and anchors it into place, so it stays at the same visual location before and after expanding/collapsing.

		unsigned short new_visibility_flags = visibility_flags ^ v;
		// calculate new total content height for new visibility flags.
		unsigned int new_content_height = 0;
		// save anchor position before and after
		int pre_anchor_y = -scroll_position;
		int post_anchor_y = -scroll_position;
		// if true, anchor has been found, so stop accumulating message heights
		bool anchored = false;

		for (int i = d_messages.size() - 1; i >= 0; --i) {
			// is message visible with previous visibility mask?
			bool pre_vis = MessageVisible(d_messages[i], visibility_flags);
			// is message visible with new visibility mask?
			bool post_vis = MessageVisible(d_messages[i], new_visibility_flags);
			unsigned int msg_height = d_messages[i].render_graphic->GetRect().height;
			// accumulate total content height for new visibility flags
			if (post_vis) new_content_height += msg_height;

			if (!anchored) {
				if (pre_vis)
					pre_anchor_y += msg_height;
				if (post_vis)
					post_anchor_y += msg_height;
				// this message is an anchor candidate since it is visible both before and after
				bool valid_anchor = pre_vis && post_vis;
				if (valid_anchor && pre_anchor_y > 0) {
					// this is the bottommost anchorable message
					anchored = true;
				}
			}
		}

		// updates scroll content height
		content_height = new_content_height;
		OnContenHeightChanged();
		// adjusts scroll position so anchor stays in place
		int scroll_delta = post_anchor_y - pre_anchor_y;
		scroll_position += scroll_delta;
		RefreshScroll();
		// set new visibility mask
		visibility_flags = new_visibility_flags;
	}

	void CaretMove(int delta, bool move_tail, bool vertical) {
		DrawableMessage& d_msg = d_messages[message_index_head];

		// Avoid moving the caret when ending the selection
		if (caret_movable) {
			if (d_msg.caret_index_head != d_msg.caret_index_tail && move_tail)
				caret_movable = false;
		} else {
			caret_movable = true;
		}

		// Move caret
		if (caret_movable) {
			int h = message_index_head;
			if (vertical) {
				message_index_head += delta;
				// Cannot move up or down. move the caret to the start or end of the current line
				if (message_index_head < 0 || message_index_head > d_messages.size() - 1)
					d_msg.caret_index_head = delta > 0 ? d_msg.caret_char_dims.size() - 1 : 0;
			} else {
				d_msg.caret_index_head += delta;
				// If the caret is out of the current line, move it to the adjacent line
				if (d_msg.caret_index_head < 0) {
					d_msg.caret_index_head = 0;
					--message_index_head;
				} else if (d_msg.caret_index_head > d_msg.caret_char_dims.size() - 1) {
					d_msg.caret_index_head = d_msg.caret_char_dims.size() - 1;
					++message_index_head;
				}
			}
			message_index_head = std::clamp(message_index_head, 0, (int)d_messages.size() - 1);
			// Skip hidden messages (lines)
			bool skipped = false;
			if (h > message_index_head) {
				for (int i = message_index_head; i >= 0; --i) {
					message_index_head = h;
					if (MessageVisible(d_messages[i], visibility_flags)) {
						message_index_head = i;
						break;
					}
				}
				skipped = true;
			} else if (h < message_index_head) {
				for (int i = message_index_head; i < d_messages.size(); ++i) {
					message_index_head = h;
					if (MessageVisible(d_messages[i], visibility_flags)) {
						message_index_head = i;
						break;
					}
				}
				skipped = true;
			}
			// Move the caret to the start or end of the current line if the previous or next lines are hidden
			if (skipped && h == message_index_head)
				d_msg.caret_index_head = delta > 0 ? d_msg.caret_char_dims.size() - 1 : 0;
		}

		// "d_next_" refers to the previous or next line
		DrawableMessage& d_next_msg = d_messages[message_index_head];
		if (&d_msg != &d_next_msg) { // About to switch lines
			if (vertical) {
				int caret_index_head = std::clamp(d_msg.caret_index_head, 0, (int)d_next_msg.caret_char_dims.size() - 1);
				// Do not let the existing selection disappear
				int nh = d_next_msg.caret_index_head, nt = d_next_msg.caret_index_tail;
				if (d_next_msg.caret_index_head == d_next_msg.caret_index_tail)
					d_next_msg.caret_index_tail = caret_index_head;
				// When moving up or down, pass the previous index to the adjacent line
				d_next_msg.caret_index_head = caret_index_head;
				if (!move_tail) {
					// Expand/collapse selection in the current line based on direction
					//  (if previously expanded, expand the other side again)
					int h = d_msg.caret_index_head;
					d_msg.caret_index_head = delta > 0 ? d_msg.caret_char_dims.size() - 1 : 0;
					bool next_selection = false;
					// Equal on both sides means the selection is cleared (including reverse selection)
					if (d_msg.caret_index_head == d_msg.caret_index_tail) {
						// Exceptions: two cases of edge selections, top-to-bottom selection.
						// + The next (last / previous or next) line has no selection
						if ((h == d_msg.caret_char_dims.size() - 1 || h == 0
								|| message_index_tail < message_index_head) && nh == nt) {
							next_selection = true;
						}
					}
					// Current line has a selection
					if (d_msg.caret_index_head != d_msg.caret_index_tail) {
						next_selection = true;
					}
					// The previous or next line has no selection
					if (next_selection && d_next_msg.caret_index_head == d_next_msg.caret_index_tail) {
						// Expand selection in adjacent line based on direction
						//  (skip this to clear the last selection)
						d_next_msg.caret_index_tail = delta < 0 ? d_next_msg.caret_char_dims.size() - 1 : 0;
					}
					BuildSelectionGraphic(d_next_msg);
				}
			} else {
				// When returning the selection, do not initialize the existing selection
				if (d_next_msg.caret_index_head == d_next_msg.caret_index_tail) {
					// Move the caret of the previous or next line to the start or end
					const auto i = delta < 0 ? d_next_msg.caret_char_dims.size() - 1 : 0;
					d_next_msg.caret_index_tail = d_next_msg.caret_index_head = i;
				}
				// Update vertical selection
				BuildSelectionGraphic(d_next_msg);
			}
		}

		// Non-selection (tail follows head)
		if (move_tail) {
			CaretEdit("", true); // Clear selection only
		}

		BuildSelectionGraphic(d_msg);

		caret_blink_shown = true;
		caret_blink_counter = 0.0f;
		caret_move = true;
	}

	void CaretEdit(const std::string& input_text, bool not_erase) {
		DrawableMessage& d_msg = d_messages[message_index_head];

		struct Selection {
			int message_index, text_tail, text_head;
		};
		std::vector<Selection> selections;
		bool add_to_selections = !input_text.empty() || !not_erase;

		// Delete the previous character when there is no selection
		if (input_text.empty() && !not_erase && message_index_tail == message_index_head
				&& d_msg.caret_index_tail == d_msg.caret_index_head) {
			// Select the previous character, then clear the selection
			CaretMove(-1, !editable, false);
		}

		bool forward_selection = false;
		if (message_index_tail < message_index_head || d_msg.caret_index_tail < d_msg.caret_index_head) {
			forward_selection = true;
		}

		// Clear/Handle selections
		if (message_index_tail != message_index_head) {
			int mt = message_index_tail, mh = message_index_head;
			// Update message_index_tail for correct vertical selection display
			message_index_tail = message_index_head;
			const unsigned int msg_start = std::min<unsigned int>(mt, mh);
			const unsigned int msg_end = std::max<unsigned int>(mt, mh);
			for (int i = msg_start; i < msg_end + 1; ++i) {
				DrawableMessage& d_i_msg = d_messages[i];
				if (add_to_selections) {
					selections.push_back({ i, d_i_msg.caret_index_tail, d_i_msg.caret_index_head });
				}
				// Clear selection
				d_i_msg.caret_index_tail = d_i_msg.caret_index_head;
				BuildSelectionGraphic(d_i_msg);
			}
		} else if (d_msg.caret_index_tail != d_msg.caret_index_head) {
			if (add_to_selections) {
				selections.push_back({ message_index_head, d_msg.caret_index_tail, d_msg.caret_index_head });
			}
			d_msg.caret_index_tail = d_msg.caret_index_head;
		}

		if (!editable) return;

		// Erase selections
		if (!selections.empty()) {
			BuildSelectionGraphic(d_msg);

			std::deque<ChatLogMessageData*> removal_messages;

			for (const auto& selection : selections) {
				const unsigned int text_start = std::min<unsigned int>(selection.text_tail, selection.text_head);
				const unsigned int text_end = std::max<unsigned int>(selection.text_tail, selection.text_head);

				DrawableMessage& d_s_msg = d_messages[selection.message_index];

				auto& text = d_s_msg.message_data->text;
				int size_count = 0;
				bool start_found = false, end_found = false;
				for (auto it = text.begin(); it != text.end();) {
					std::u32string t_u32 = Utils::DecodeUTF32(it->first);

					// Get the index of the substring
					const unsigned int sub_start = std::max<int>(text_start - size_count, 0);
					const unsigned int sub_end = std::min<int>((int)t_u32.size(), text_end - size_count);

					size_count += t_u32.size();

					if (size_count > text_start) start_found = true;
					if (size_count > text_end) end_found = true;

					// Skip the initial unselected part
					if (!start_found && !end_found) {
						++it;
						continue;
					}

					int length = sub_end - sub_start;

					t_u32.erase(sub_start, length);

					if (forward_selection) {
						// Set the caret to 0
						//  (then use CaretMove to move the caret to the previous line when removing the message)
						d_s_msg.caret_index_tail = d_s_msg.caret_index_head -= length;
					}

					if (t_u32.size() > 0 || text.size() == 1) {
						it->first = Utils::EncodeUTF(t_u32);
						++it;
					} else {
						it = text.erase(it);
						if (it == text.end()) break;
					}

					// Skip the remaining unselected part
					if (start_found && end_found) break;
				}

				if (text.size() == 1 && text.front().first.empty()) {
					d_s_msg.message_data->remove_message = true;
					removal_messages.push_back(d_s_msg.message_data.get());
				}

				d_s_msg.dirty = true;
			}

			// Concatenate the two remaining segments after deleting line selections
			int last_text_length = 0;
			if (selections.size() > 1) {
				auto& d_first_msg = d_messages[selections.front().message_index];
				const auto& d_last_msg = d_messages[selections.back().message_index];

				auto& first_text = d_messages[selections.front().message_index].message_data->text;
				const auto& last_text = d_last_msg.message_data->text;

				for (const auto& t : last_text)
					last_text_length += Utils::DecodeUTF32(t.first).size();

				// This is the empty line left by the previous deletion that needs to be concatenated
				if (d_first_msg.message_data->remove_message) {
					d_first_msg.message_data->remove_message = false;
					removal_messages.pop_front();
					first_text.clear();
				}

				if (last_text_length > 0 || first_text.size() == 0) {
					first_text.reserve(first_text.size() + last_text.size());
					std::move(last_text.begin(), last_text.end(), std::back_inserter(first_text));
				}

				// Update the first message's caret_char_dims
				//  to avoid using the old position when moving (CaretMove) the caret to the previous line.
				BuildMessageGraphic(d_first_msg);

				d_last_msg.message_data->remove_message = true;
				removal_messages.push_back(d_last_msg.message_data.get());
			}

			if (!removal_messages.empty()) {
				if (last_text_length == 0) {
					// Do not delete the last (empty) message immediately. it needs to be deleted again
					removal_messages.back()->remove_message = false;
				}
				for (ChatLogMessageData* p : removal_messages) {
					if (d_messages.size() == 1) break;
					if (forward_selection && p->remove_message)
						CaretMove(-1, true, false);
					if (p->remove_message)
						RemoveLogEntry(p); // The reference to d_msg will change
					else
						p->remove_message = true;
				}
				if (forward_selection)
					CaretMove(-last_text_length, true, false);
			}
		}

		// Insert without requiring selections
		if (!input_text.empty()) {
			size_t start = 0, end = 0;
			for (;;) {
				end = input_text.find('\n', start);
				std::string sub_input_text = input_text.substr(start, end - start);

				// Text insertion
				if (!sub_input_text.empty()) {
					DrawableMessage& d_msg = d_messages[message_index_head];

					auto& text = d_msg.message_data->text;
					int size_count = 0;

					for (auto it = text.begin(); it != text.end(); ++it) {
						std::u32string t_u32 = Utils::DecodeUTF32(it->first);

						const int index = d_msg.caret_index_head - size_count;
						if (index >= 0 && index <= t_u32.size()) {
							std::u32string input_u32 = Utils::DecodeUTF32(sub_input_text);
							std::u32string fits = input_u32.substr(0, CHATLOG_MAX_CHARS_INPUT - GetLength());
							t_u32.insert(index, fits);
							it->first = Utils::EncodeUTF(t_u32);
							d_msg.caret_index_tail = d_msg.caret_index_head += fits.size();
							break;
						}

						size_count += t_u32.size();
					}

					d_msg.message_data->remove_message = false;
					d_msg.dirty = true;
				}

				// Line break insertion
				if (end != std::string::npos && GetLength() < CHATLOG_MAX_CHARS_INPUT) {
					// Update d_msg when adding a new line in multiple selections
					DrawableMessage& d_msg = d_messages[message_index_head];

					auto first_msg = std::make_unique<ChatLogMessageData>(ChatLogText{}, Chat::CV_LOCAL, "", true);
					auto& first_text = first_msg->text;

					auto& second_text = d_msg.message_data->text;
					auto new_second_text = ChatLogText{};
					int size_count = 0;

					// Split a line into two at the caret index
					for (auto it = second_text.begin(); it != second_text.end(); ++it) {
						std::u32string t_u32 = Utils::DecodeUTF32(it->first);

						const int index = d_msg.caret_index_head - size_count;
						if (index >= 0 && index <= t_u32.size()) {
							auto first_t = *it;
							first_t.first = Utils::EncodeUTF(t_u32.substr(0, index));
							first_text.push_back(std::move(first_t));

							it->first = Utils::EncodeUTF(t_u32.substr(index));
							for (auto it2 = it; it2 != second_text.end(); ++it2)
								new_second_text.push_back(std::move(*it2));
							// Prevent adding many empty text
							if (new_second_text.front().first.empty() && new_second_text.size() > 1)
								new_second_text.erase(new_second_text.begin());

							break;
						} else {
							first_text.push_back(std::move(*it));
						}

						size_count += t_u32.size();
					}
					second_text = new_second_text;

					d_msg.message_data->remove_message = false;
					BuildMessageGraphic(d_msg);

					// The reference to d_msg will change
					AddLogEntry(d_msg.message_data.get(), first_msg);

					CaretMove(1, true, true);
				} else {
					break;
				}

				start = end + 1;
			}
		}

		caret_blink_shown = true;
		caret_blink_counter = 0.0f;
		caret_move = true;
	}

	void CaretCopy(std::string& output_text, bool copy_all) {
		struct Selection {
			int message_index, text_tail, text_head;
		};
		std::vector<Selection> selections;

		int mt = message_index_tail, mh = message_index_head;
		if (copy_all) mt = d_messages.size() - 1, mh = 0;
		const unsigned int msg_start = std::min<unsigned int>(mt, mh);
		const unsigned int msg_end = std::max<unsigned int>(mt, mh);
		for (int i = msg_start; i < msg_end + 1; ++i) {
			const DrawableMessage& d_i_msg = d_messages[i];
			if (!MessageVisible(d_i_msg, visibility_flags)) continue;
			int t = d_i_msg.caret_index_tail, h = d_i_msg.caret_index_head;
			if (copy_all) t = d_i_msg.caret_char_dims.size() - 1, h = 0;
			selections.push_back({ i, t, h });
		}

		if (selections.empty()) return;

		for (auto it = selections.begin(); it != selections.end(); ++it) {
			const auto& selection = *it;

			const unsigned int text_start = std::min<unsigned int>(selection.text_tail, selection.text_head);
			const unsigned int text_end = std::max<unsigned int>(selection.text_tail, selection.text_head);

			DrawableMessage& d_s_msg = d_messages[selection.message_index];

			auto& text = d_s_msg.message_data->text;
			int size_count = 0;
			bool start_found = false, end_found = false;
			for (auto it = text.begin(); it != text.end();) {
				std::u32string t_u32 = Utils::DecodeUTF32(it->first);

				const unsigned int sub_start = std::max<int>(text_start - size_count, 0);
				const unsigned int sub_end = std::min<int>((int)t_u32.size(), text_end - size_count);

				size_count += t_u32.size();

				if (size_count > text_start) start_found = true;
				if (size_count > text_end) end_found = true;

				if (!start_found && !end_found) {
					++it;
					continue;
				}

				t_u32 = t_u32.substr(sub_start, sub_end - sub_start);
				output_text.append(Utils::EncodeUTF(t_u32));

				++it;

				if (start_found && end_found) break;
			}

			if (std::next(it) != selections.end())
				output_text.append("\n");
		}
	}

	void CaretEraseAll() {
		while (!d_messages.empty()) RemoveLogEntry();
		message_index_tail = message_index_head = 0;
		AddLogEntry(std::make_unique<ChatLogMessageData>(ChatLogText({{"", 0}}),
			Chat::CV_LOCAL, "", true));
		scroll_position = 0;
		RefreshScroll();
	}

	std::pair<int, int> CaretGetLineColumn() {
		DrawableMessage& d_msg = d_messages[message_index_head];
		return std::make_pair(message_index_head, d_msg.caret_index_head);
	}
};

/**
 * ChatBox
 */

class DrawableChatBox : public Drawable {
	// Design parameters 1
	const unsigned int panel_frame = 4;
	const unsigned int notification_log_width = SCREEN_TARGET_WIDTH;
	const unsigned int notification_log_height = SCREEN_TARGET_HEIGHT * 0.275;
	const unsigned int chatbox_width = SCREEN_TARGET_WIDTH * 0.725;
	const unsigned int chatbox_height = SCREEN_TARGET_HEIGHT;
	const unsigned int status_height = 20;
	const unsigned int chatlog_left = 2;
	const unsigned int type_margin = 4;
	const unsigned int type_padding_x = 6;
	const unsigned int type_maxheight = chatbox_height / 2.618;
	// 2
	const unsigned int notification_log_top = SCREEN_TARGET_HEIGHT - notification_log_height;
	const unsigned int chatbox_inner_width = chatbox_width - panel_frame * 2;
	const unsigned int chatbox_inner_height = chatbox_height - panel_frame * 2;
	const unsigned int log_scroll_delta = chatbox_inner_height / 16;
	const unsigned int type_width = chatbox_inner_width - type_margin - type_padding_x;
	// Constructor start

	unsigned int screen_width = SCREEN_TARGET_WIDTH;
	unsigned int screen_height = SCREEN_TARGET_HEIGHT;
	unsigned int chatbox_top = 0;
	unsigned int chatbox_left = 0;
	unsigned int chatlog_height = 0;
	unsigned int type_top_rel = 0; // relative
	unsigned int type_left = 0;
	unsigned int type_height = 0;

	DrawableChatLog d_notification_log;
	bool notification_log_shown = true;

	Window_Base back_panel; // Background image
	DrawableOnlineStatus d_status;
	DrawableChatLog d_log;
	DrawableChatLog d_type;

	bool focused = false;
	bool copylog = false;
	bool vertical = false;
	bool immersive_mode_flag = false;
	bool split_screen_flag = false;

	void UpdatePositionsAndSizes() {
		if (!split_screen_flag) {
			screen_width = Player::screen_width;
			screen_height = Player::screen_height;
		}

		/** left */
		chatbox_left = screen_width - chatbox_width;
		const unsigned int chatbox_inner_left = chatbox_left + panel_frame;
		type_left = immersive_mode_flag
			? chatbox_inner_left + chatlog_left
			: chatbox_inner_left + type_margin + type_padding_x;

		back_panel.SetX(chatbox_left);
		d_status.SetX(chatbox_inner_left);
		d_log.SetX(chatbox_inner_left + chatlog_left);
		d_type.SetX(type_left);

		/** top */
		chatbox_top = screen_height - chatbox_height;

		type_height = focused ? d_type.GetContentHeight() : 0;
		if (type_height > type_maxheight)
			type_height = type_maxheight;

		chatlog_height = chatbox_inner_height - status_height - type_height - (focused ? type_margin : 0);
		type_top_rel = status_height + chatlog_height;

		back_panel.SetY(chatbox_top);
		d_status.SetY(chatbox_top);
		d_log.SetY(chatbox_top + status_height);
		d_type.SetY(chatbox_top + type_top_rel + type_margin);

		/** height */
		d_log.SetHeight(chatlog_height);
		d_type.SetHeight(type_height);
	}

	void UpdateTypePanel() {
		if (d_type.IsVisible()) {
			// SetCursorRect for some reason already has a padding of 8px relative to the window, so we fix it
			const unsigned int fix = 4;
			back_panel.SetCursorRect(
				Rect(-fix + type_margin, type_top_rel - type_margin - fix, type_width, type_height + type_margin + fix));
		} else {
			back_panel.SetCursorRect(Rect(0, 0, 0, 0));
		}
	}

	void UpdateVisibility() {
		bool is_visible = split_screen_flag ? true : focused;
		if (notification_log_shown)
			d_notification_log.SetVisible(split_screen_flag ? is_visible : !focused);
		this->SetVisible(is_visible);
		if (!immersive_mode_flag)
			back_panel.SetVisible(is_visible);
		d_status.SetVisible(is_visible);
		d_log.SetVisible(is_visible);
	}

public:
	DrawableChatBox()
		: Drawable(Priority::Priority_Maximum, Drawable::Flags::Global),
		d_notification_log(0, notification_log_top, notification_log_width, notification_log_height, 0),
		back_panel(0, 0, chatbox_width, chatbox_height, Drawable::Flags::Global),
		d_status(0, 0, chatbox_inner_width, status_height),
		d_log(0, 0, chatbox_inner_width, 0, 1),
		d_type(0, 0, type_width - type_padding_x, 0, 2)
	{
		DrawableMgr::Register(this);

		back_panel.SetZ(Priority::Priority_Maximum - 1);
		back_panel.SetOpacity(240);

		d_notification_log.SetOverlayMode(true, true);
		d_notification_log.ToggleVisibilityFlag(Chat::CV_VERBOSE);

		d_type.SetMode(true, true);
		d_type.OnContenHeightChanged = [this]() {
			UpdatePositionsAndSizes();
			UpdateTypePanel();
		};
		d_type.OnCaretMoved = [this](Rect caret_dims) {
			DisplayUi->SetTextInputRect(
				type_left + caret_dims.x,
				chatbox_top + type_top_rel + caret_dims.y + caret_dims.height
			);
		};

		UpdatePositionsAndSizes();
		UpdateTypePanel();

		SetImmersiveMode(GMI().GetConfig().client_chat_immersive_mode.Get());
		SetNotificationLog(GMI().GetConfig().client_chat_notifications.Get());
		SetFocus(false);
	}

	void SetFocus(bool focused) {
		this->focused = focused;
		d_type.SetVisible(focused);
		UpdateVisibility();
		UpdatePositionsAndSizes();
		UpdateTypePanel();
		d_log.ShowScrollBar(focused);
		if (focused) {
			d_type.CaretMove(0, true, false); // reset caret blink
			DisplayUi->StartTextInput();
		} else {
			DisplayUi->StopTextInput();
		}
	}

	void SetImmersiveMode(bool enabled) {
		GMI().GetConfig().client_chat_immersive_mode.Set(enabled);
		immersive_mode_flag = enabled;
		back_panel.SetVisible(!enabled);
		d_log.SetOverlayMode(enabled);
		d_type.SetOverlayMode(enabled);
		if (!immersive_mode_flag)
			back_panel.SetWindowskin(Cache::SystemOrBlack());
		UpdatePositionsAndSizes();
		UpdateTypePanel();
	}

	void ToggleImmersiveMode() {
		SetImmersiveMode(!immersive_mode_flag);
	}

	void SetSplitScreenMode(bool enable, bool vertical, bool toggle) {
		if (toggle && split_screen_flag == enable && this->vertical == vertical) {
			enable = !enable;
		}
		split_screen_flag = enable;
		if (split_screen_flag) {
			if (!vertical) {
				screen_width = Player::screen_width + chatbox_width;
				screen_height = Player::screen_height;
				GMI().GetConfig().client_chat_splitscreen_mode.Set(1);
			} else {
				screen_width = Player::screen_width;
				screen_height = Player::screen_height + chatbox_height;
				GMI().GetConfig().client_chat_splitscreen_mode.Set(2);
			}
			this->vertical = vertical;
		} else {
			screen_width = Player::screen_width;
			screen_height = Player::screen_height;
			this->vertical = false;
			GMI().GetConfig().client_chat_splitscreen_mode.Set(0);
		}
		DisplayUi->ChangeDisplaySurfaceResolution(screen_width, screen_height);
		UpdateVisibility();
		UpdatePositionsAndSizes();
	}

	void RefreshTheme() {
		if (!immersive_mode_flag)
			back_panel.SetWindowskin(Cache::SystemOrBlack());
		d_notification_log.RefreshTheme();
		d_status.RefreshTheme();
		d_log.RefreshTheme();
		d_type.RefreshTheme();
	}

	void UpdateDisplaySurfaceResolution() {
		if (split_screen_flag) {
			DisplayUi->ChangeDisplaySurfaceResolution(screen_width, screen_height);
		}
		UpdatePositionsAndSizes();
	}

	void Draw(Bitmap& dst) { }

	void AddNotificationLogEntry(std::unique_ptr<ChatLogMessageData>&& msg) {
		d_notification_log.AddLogEntry(std::forward<std::unique_ptr<ChatLogMessageData>>(msg));
	}

	void SetNotificationLog(bool enable) {
		GMI().GetConfig().client_chat_notifications.Set(enable);
		notification_log_shown = enable;
		d_notification_log.SetVisible(enable);
	}

	void ToggleNotificationLog() {
		SetNotificationLog(!notification_log_shown);
		Graphics::GetStatusTextOverlay().ShowText(
			notification_log_shown ? "Notifications shown" : "Notifications hidden"
		);
	}

	void SetStatusConnection(bool conn, bool connecting) {
		d_status.SetConnectionStatus(conn, connecting);
	}

	void SetStatusRoom(unsigned int room_id) {
		d_status.SetRoomStatus(room_id);
	}

	void SetStatusProgress(unsigned int percent) {
		d_status.SetProgressStatus(percent);
	}

	void AddLogEntry(std::unique_ptr<ChatLogMessageData>&& msg) {
		d_log.AddLogEntry(std::forward<std::unique_ptr<ChatLogMessageData>>(msg));
	}

	void ScrollUp() {
		if (!copylog && d_type.GetVisibleMessageCount() == 1)
			d_log.Scroll(+log_scroll_delta);
	}

	void ScrollDown() {
		if (!copylog && d_type.GetVisibleMessageCount() == 1)
			d_log.Scroll(-log_scroll_delta);
	}

	unsigned short GetVisibilityFlags() {
		return d_log.GetVisibilityFlags();
	}

	void ToggleVisibilityFlag(VisibilityType v) {
		d_notification_log.ToggleVisibilityFlag(v);
		d_log.ToggleVisibilityFlag(v);
	}

	bool Cancel() {
		if (copylog) {
			d_log.SetMode(false, false);
			d_type.SetMode(true, true);
			d_type.CaretMove(0, true, false); // reset caret blink
			copylog = false;
			return true;
		}
		return false;
	}

	void CaretMove(int delta, bool move_tail, bool vertical) {
		if (delta < 0 && !move_tail && vertical && !copylog) {
			auto ln_col = d_type.CaretGetLineColumn();
			if (ln_col.first == 0 && ln_col.second == 0) {
				d_log.SetMode(true, false);
				d_log.CaretMove(0, true, false); // reset caret blink
				d_type.SetMode(false, false);
				copylog = true;
				return;
			}
		}
		if (copylog) {
			d_log.CaretMove(delta, move_tail, vertical);
		} else {
			bool move = false;
			if (d_type.GetVisibleMessageCount() > 1)
				move = true;
			else if (!move_tail && vertical)
				move = true;
			else if (!vertical)
				move = true;
			if (move) d_type.CaretMove(delta, move_tail, vertical);
		}
	}

	void CaretEdit(const std::string& input_text, bool not_erase) {
		if (copylog)
			d_log.CaretEdit(input_text, not_erase);
		else
			d_type.CaretEdit(input_text, not_erase);
	}

	void CaretCopy(std::string& output_text) {
		if (copylog)
			d_log.CaretCopy(output_text, false);
		else
			d_type.CaretCopy(output_text, false);
	}

	void GetTypedText(std::string& output_text) {
		if (!copylog) {
			d_type.CaretCopy(output_text, true);
			d_type.CaretEraseAll();
		}
	}
};

/**
 * ChatUi
 */

std::unique_ptr<DrawableChatBox> chat_box;
bool initialized = false;
int update_counter = 0;
int counter_chatbox = 0;

/** for commands */
VisibilityType chat_visibility = Chat::CV_LOCAL;
bool cheat_flag = false;

/** debug text overlay */
bool dto_downloading_flag = false;
std::string dto_downloading_text;

void AddLogEntry(ChatLogText&& t, VisibilityType v, std::string sys_name) {
	chat_box->AddLogEntry(std::make_unique<ChatLogMessageData>(
		std::forward<ChatLogText>(t), v, std::forward<std::string>(sys_name), true));
}

void AddNotificationLogEntry(ChatLogText&& t, VisibilityType v, std::string sys_name) {
	chat_box->AddNotificationLogEntry(std::make_unique<ChatLogMessageData>(
		std::forward<ChatLogText>(t), v, std::forward<std::string>(sys_name), false));
}

// => Default
void PrintD(std::string message, bool notify_add = false, VisibilityType v = Chat::CV_LOCAL) {
	AddLogEntry(ChatLogText{{message, tcfg.color_print_message}}, v, "");
	if (notify_add)
		AddNotificationLogEntry(ChatLogText{{message, tcfg.color_print_message}}, v, "");
}

// => Label ...
void PrintL(std::string label, std::string message = "", bool notify_add = false) {
	AddLogEntry(ChatLogText{
		{label, tcfg.color_print_label}, {message, tcfg.color_print_label_message}
	}, Chat::CV_LOCAL, "");
	if (notify_add) {
		AddNotificationLogEntry(ChatLogText{
			{label, tcfg.color_print_label}, {message, tcfg.color_print_label_message}
		}, Chat::CV_LOCAL, "");
	}
}

// => [Client]: ...
void PrintC(std::string message, bool notify_add = false) {
	PrintL("[Client]: ", std::move(message), notify_add);
}

void ShowWelcome() {
	PrintD("• IME input now supported!");
	PrintD("  (for CJK characters, etc.)");
	PrintD("• You can now copy and");
	PrintD("  paste from type box.");
	PrintD("• SHIFT+[←, →] to select text.");
#ifdef EMSCRIPTEN
	PrintD("• In file scene (Savegame),");
	PrintD("  press SHIFT to upload.");
	PrintD("  (File uploaded locally only)");
#endif
	PrintD("• Type !help to list commands.");
	PrintD("―――");
	PrintL("[F3]: ", "hide/show notifications.");
	PrintL("[TAB]: ", "focus/unfocus.");
	PrintL("[↑, ↓]: ", "scroll.");
	PrintD("―――");
	PrintD("v" + Version::GetVersionString(true, true));
}

void ShowUsage(Strfnd& fnd) {
	PrintD("―――");
	PrintD("Usage:");
	PrintD("[...] Optional | <...> Required");
	PrintD("―――");
	std::string doc_name = fnd.next(" ");
	if (doc_name.empty()) {
		PrintL("<!server, !srv> ", "[on, off]");
		PrintD("- turn on/off the server");
		PrintL("<!crypt> ", "[password, <empty>]");
		PrintD("- configure connection encryption");
		PrintL("<!connect, !c> ", "[address, <empty>]");
		PrintD("- connect to the server");
		PrintL("<!disconnect, !d>");
		PrintD("- disconnect from server");
		PrintL("!name ", "[text, <unknown>]");
		PrintD("- change chat name");
		PrintL("!chat [LOCAL, GLOBAL, CRYPT] ", "[CRYPT Password]");
		PrintD("- switch visibility to chat");
		PrintL("!log ", "[LOCAL, GLOBAL, CRYPT]");
		PrintD("- toggle visibility");
		PrintL("<!immersive, !imm>");
		PrintD("- toggle the immersive mode");
		PrintL("<!splitscreen, !ss> ", "[vertically, v]");
		PrintD("- toggle the split-screen mode");
		PrintL("<!debugtext, !dt> ");
		PrintD("- print debug text");
		PrintL("<!debugtextoverlay, !dto> ", "...");
		PrintD("- see !help debugtextoverlay");
	} else if (doc_name == "cheat") {
		PrintL("!cheat");
		PrintD("- Toggle cheat mode");
		PrintD("(The following commands depend on this mode)");
		PrintL("!getvar <id> | !setvar <id> <value>");
		PrintD("- Get/Set variables");
		PrintL("!getsw <id> | !setsw <id> <0, 1>");
		PrintD("- Get/Set switches");
		PrintL("!debug");
		PrintD("- Enable TestPlay mode");
	} else if (doc_name == "debugtextoverlay") {
		PrintL("<..., !dto> ", "[player, p]");
		PrintD("- Toggle player status");
		PrintL("<..., !dto> ", "<downloading, d>");
		PrintD("- Toggle downloading status");
	} else {
		PrintD("No entry for " + doc_name);
	}
}

bool SetChatVisibility(std::string visibility_name) {
	auto it = Chat::VisibilityValues.find(visibility_name);
	if (it != Chat::VisibilityValues.end()) {
		chat_visibility = it->second;
		return true;
	}
	return false;
}

void SendKeyHash() {
	if (chat_visibility == Chat::CV_CRYPT) {
		std::string key = GMI().GetConfig().client_chat_crypt_key.Get();
		std::istringstream key_iss(key);
		// send a hash integer to help the server to search for clients with the same key
		GMI().SendChatMessage(static_cast<int>(chat_visibility), "", Utils::CRC32(key_iss));
	}
}

void GeneratePasswordKey(std::string password, std::function<void(std::string)> callback) {
	auto GenerateKey = [password, callback]() {
		OutputMt::Info("CRYPT: Generating encryption key ...");
		std::string key;
		CryptoError err = CryptoGetPasswordBase64Hash(password, key);
		if (err == CryptoError::CE_NO_ERROR) {
			callback(key);
			OutputMt::Info("CRYPT: Done");
		} else {
			OutputMt::Warning("CRYPT: Key generation failed. err = {}", CryptoErrString(err));
		}
	};
#ifndef EMSCRIPTEN
	std::thread(GenerateKey).detach();
#else
	GenerateKey();
#endif
}

void ToggleCheat() {
	cheat_flag = !cheat_flag;
	PrintC("Cheat: " + std::string(cheat_flag == true ? "enabled" : "disabled"));
	if (cheat_flag)
		PrintC("You can type !cheat to turn it off.");
	else if (Player::debug_flag) {
		if (Scene::Find(Scene::SceneType::Debug) != nullptr)
			Scene::Pop();
		Player::debug_flag = false;
		PrintC("TestPlay mode: disabled");
	}
}

void SetFocus(bool focused) {
	if (!focused && chat_box->Cancel()) return;
	Input::SetGameFocus(!focused);
	chat_box->SetFocus(focused);
	if (focused && Player::debug_flag && !cheat_flag) {
		PrintC("[TestPlay] The cheat mode is being toggled");
		ToggleCheat();
	}
}

void Update() {
	if (!initialized) {
		++update_counter;

		if (counter_chatbox == 0) {
			auto ptr = Scene::Find(Scene::SceneType::Title);
			if (!ptr) ptr = Scene::Find(Scene::SceneType::Map);
			if (!ptr) ptr = Scene::Find(Scene::SceneType::GameBrowser);
			if (ptr) counter_chatbox = update_counter;
		}

		if (counter_chatbox > 0) {
			int counter = update_counter - counter_chatbox;
			if (counter == 7) {
				chat_box = std::make_unique<DrawableChatBox>();
				ShowWelcome();
				SetChatVisibility(GMI().GetConfig().client_chat_visibility.Get());
			} else if (counter == 8) { // 8: do something after the original screen adjustment is completed
				int splitscreen_mode = GMI().GetConfig().client_chat_splitscreen_mode.Get();
				if (splitscreen_mode)
					chat_box->SetSplitScreenMode(splitscreen_mode, splitscreen_mode == 2, false);
				initialized = true;
			}
		}
	}

	if (chat_box == nullptr) return;

	if (dto_downloading_flag) {
		Graphics::GetDebugTextOverlay().UpdateItem("99_downloading", dto_downloading_text);
	}

	// Focus
	if (Input::IsTriggered(Input::InputButton::TOGGLE_CHAT)) {
		SetFocus(true);
	} else if (Input::IsExternalTriggered(Input::InputButton::TOGGLE_CHAT) ||
			Input::IsExternalTriggered(Input::InputButton::KEY_ESCAPE)) {
		SetFocus(false);
	}

	// Scroll
	if (!Input::IsExternalPressed(Input::InputButton::SHIFT)) {
		if (Input::IsExternalPressed(Input::InputButton::KEY_UP))
			chat_box->ScrollUp();
		if (Input::IsExternalPressed(Input::InputButton::KEY_DOWN))
			chat_box->ScrollDown();
	}

	// Toggle notification log
	if (Input::IsTriggered(Input::InputButton::TOGGLE_NOTIFICATIONS)) {
		chat_box->ToggleNotificationLog();
	}

	// Input and paste
	std::string input_text = Input::GetExternalTextInput();
	if (Input::IsExternalTriggered(Input::InputButton::KEY_V) && Input::IsExternalPressed(Input::InputButton::KEY_CTRL))
		input_text = DisplayUi->GetClipboardText();
	if (input_text.size() > 0)
		chat_box->CaretEdit(input_text, false);
	if (Input::IsExternalPressed(Input::InputButton::SHIFT) && Input::IsExternalRepeated(Input::InputButton::KEY_RETURN))
		chat_box->CaretEdit("\n", false); // New line

	// Erase
	if (Input::IsExternalRepeated(Input::InputButton::KEY_BACKSPACE))
		chat_box->CaretEdit("", false);

	// Copy
	if (Input::IsExternalTriggered(Input::InputButton::KEY_C) && Input::IsExternalPressed(Input::InputButton::KEY_CTRL)) {
		std::string selected;
		chat_box->CaretCopy(selected);
		DisplayUi->SetClipboardText(selected);
	}

	// CaretMove
	if (Input::IsExternalRepeated(Input::InputButton::KEY_LEFT))
		chat_box->CaretMove(-1, !Input::IsExternalPressed(Input::InputButton::SHIFT), false);
	if (Input::IsExternalRepeated(Input::InputButton::KEY_RIGHT))
		chat_box->CaretMove(1, !Input::IsExternalPressed(Input::InputButton::SHIFT), false);
	if (Input::IsExternalRepeated(Input::InputButton::KEY_UP))
		chat_box->CaretMove(-1, !Input::IsExternalPressed(Input::InputButton::SHIFT), true);
	if (Input::IsExternalRepeated(Input::InputButton::KEY_DOWN))
		chat_box->CaretMove(1, !Input::IsExternalPressed(Input::InputButton::SHIFT), true);

	// Enter
	while (Input::IsExternalTriggered(Input::InputButton::KEY_RETURN)
			&& !Input::IsExternalPressed(Input::InputButton::SHIFT)) {
		std::string text;
		chat_box->GetTypedText(text);
		if (text.empty()) break;

		Strfnd fnd(text);
		std::string command = fnd.next(" ");
		// command: !server
		if (command == "!server" || command == "!srv") {
#ifndef EMSCRIPTEN
			std::string option = fnd.next(" ");
			if (option == "on") {
				Server().Start();
				PrintC("Server: on");
			} else if (option == "off") {
				Server().Stop();
				PrintC("Server: off");
			}
#endif
		// command: !crypt
		} else if (command == "!crypt") {
			std::string password = fnd.next("");
			if (password != "") {
				auto Reminder = []() {
					if (GMI().IsActive())
						OutputMt::Info("You need to reconnect after setting up the encryption.");
				};
				if (password != "<empty>") {
					GeneratePasswordKey(password, [Reminder](std::string key) {
						GMI().GetConfig().client_crypt_key.Set(key);
						Reminder();
					});
				} else {
					GMI().GetConfig().client_crypt_key.Set("");
					PrintC("Encryption has been disabled.");
					Reminder();
				}
			} else {
				PrintC(std::string("Encryption: ")
					+ (GMI().GetConfig().client_crypt_key.Get().empty() ? "disabled" : "enabled"));
			}
		// command: !connect
		} else if (command == "!connect" || command == "!c") {
			std::string address = fnd.next("");
			if (address != "")
				GMI().SetRemoteAddress(address == "<empty>" ? "" : address);
			GMI().Connect();
		// command: !disconnect
		} else if (command == "!disconnect" || command == "!d") {
			GMI().Disconnect();
		// command: !name
		} else if (command == "!name") {
			std::string name = fnd.next(" ");
			if (name != "")
				GMI().SetChatName(name == "<unknown>" ? "" : name);
			name = GMI().GetChatName();
			PrintC("Name: " + (name == "" ? "<unknown>" : name));
		// command: !chat
		} else if (command == "!chat") {
			std::string visibility_name = fnd.next(" ");
			if (visibility_name != "") {
				if (SetChatVisibility(visibility_name))
					GMI().GetConfig().client_chat_visibility.Set(visibility_name);
			}
			PrintC("Visibility: " + \
				Chat::VisibilityNames.find(chat_visibility)->second);
			if (visibility_name == "CRYPT") {
				std::string chat_crypt_password = fnd.next(" ");
				if (chat_crypt_password != "") {
					GeneratePasswordKey(chat_crypt_password, [](std::string key) {
						GMI().GetConfig().client_chat_crypt_key.Set(key);
						SendKeyHash();
					});
				}
			}
		// command: !log
		} else if (command == "!log") {
			auto it = Chat::VisibilityValues.find(fnd.next(""));
			if (it != Chat::VisibilityValues.end())
				chat_box->ToggleVisibilityFlag(it->second);
			std::string flags = "";
			for (const auto& it : Chat::VisibilityNames)
				if ((chat_box->GetVisibilityFlags() & it.first) > 0)
					flags += it.second + " ";
			if (flags.size() > 0)
				flags.erase(flags.size() - 1);
			PrintC("Flags: " + flags);
		// command: !immersive
		} else if (command == "!immersive" || command == "!imm") {
			chat_box->ToggleImmersiveMode();
		// command: !splitscreen
		} else if (command == "!splitscreen" || command == "!ss") {
			std::string vert = fnd.next(" ");
			chat_box->SetSplitScreenMode(true, vert == "vertically" || vert == "v" ? true : false, true);
		// command: !cheat
		} else if (command == "!cheat") {
			ToggleCheat();
		// command: !debug
		} else if (command == "!debug" && cheat_flag) {
			Player::debug_flag = true;
			PrintC("TestPlay mode: enabled");
			PrintC("You can focus on the ChatUi by selecting the 'Chat' in the debug menu.");
			Scene::Push(std::make_shared<Scene_Debug>());
		// command: !getvar
		} else if (command == "!getvar" && cheat_flag) {
			std::string var_id = fnd.next(" ");
			PrintC("getvar #" + var_id + " = " + std::to_string(Main_Data::game_variables->Get(atoi(var_id.c_str()))));
		// command: !setvar
		} else if (command == "!setvar" && cheat_flag) {
			std::string var_id = fnd.next(" ");
			Main_Data::game_variables->Set(atoi(var_id.c_str()), atoi(fnd.next(" ").c_str()));
			Game_Map::SetNeedRefresh(true);
			PrintC("setvar #" + var_id + " = " + std::to_string(Main_Data::game_variables->Get(atoi(var_id.c_str()))));
		// command: !getsw
		} else if (command == "!getsw" && cheat_flag) {
			std::string sw_id = fnd.next(" ");
			PrintC("getsw #" + sw_id + " = " + (Main_Data::game_switches->Get(atoi(sw_id.c_str())) ? "on" : "off"));
		// command: !setsw
		} else if (command == "!setsw" && cheat_flag) {
			std::string sw_id = fnd.next(" ");
			Main_Data::game_switches->Set(atoi(sw_id.c_str()), atoi(fnd.next(" ").c_str()));
			Game_Map::SetNeedRefresh(true);
			PrintC("setsw #" + sw_id + " = " + (Main_Data::game_switches->Get(atoi(sw_id.c_str())) ? "on" : "off"));
		// command: !debugtext
		} else if (command == "!debugtext" || command == "!dt") {
			Output::InfoStr(GMI().GetDebugText(Multiplayer::DT_DEFAULT));
		// command: !debugtextoverlay
		} else if (command == "!debugtextoverlay" || command == "!dto") {
			std::string name = fnd.next(" ");
			if (name == "player" || name == "p") {
				GMI().ToggleDebugTextOverlayMode(Multiplayer::DT_PLAYER_FULL);
			} else if (name == "downloading" || name == "d") {
				dto_downloading_flag = !dto_downloading_flag;
				if (dto_downloading_flag)
					Graphics::GetDebugTextOverlay().ShowItem("99_downloading");
				else
					Graphics::GetDebugTextOverlay().HideItem("99_downloading");
				PrintC(std::string("DebugTextOverlay: ") + (dto_downloading_flag ? "enabled" : "disabled"));
			} else
				GMI().ToggleDebugTextOverlayMode(Multiplayer::DT_PLAYER_A);
		// command: !help
		} else if (command == "!help") {
			ShowUsage(fnd);
		} else {
			if (text != "") {
				if (chat_visibility == Chat::CV_CRYPT) {
					std::string key = GMI().GetConfig().client_chat_crypt_key.Get();
					std::vector<char> cipher_data;
					CryptoError err = CryptoEncryptText(key, text, cipher_data);
					if (err == CryptoError::CE_NO_ERROR) {
						GMI().SendChatMessage(static_cast<int>(chat_visibility),
							std::string(cipher_data.data(), cipher_data.size()));
					} else {
						Output::Warning("CRYPT: Encrypt failed. err = {}", CryptoErrString(err));
					}
				} else
					GMI().SendChatMessage(static_cast<int>(chat_visibility), text);
			}
		}

		break;
	}
}

} // end of namespace

/**
 * External access
 */

static ChatUi _instance;

ChatUi& ChatUi::Instance() {
	return _instance;
}

void ChatUi::Refresh() {
	if (chat_box == nullptr) return;
	// Do not add thread lock here to avoid deadlock
	chat_box->RefreshTheme();
}

void ChatUi::Update() {
	::Update();
}

void ChatUi::OnResolutionChange() {
	if (chat_box == nullptr) return;
	chat_box->UpdateDisplaySurfaceResolution();
}

void ChatUi::SetFocus(bool focused) {
	if (chat_box == nullptr) return;
	::SetFocus(focused);
}

void ChatUi::GotMessage(int visibility, int room_id,
		std::string name, std::string message, std::string sys_name) {
	if (chat_box == nullptr) return;
	if (name.size() > 16) name = "<unknown>";
	if (Utils::DecodeUTF32(message).size() > CHATLOG_MAX_CHARS_INPUT) {
		Output::InfoStr("Sender's message too long, ignored.");
		return;
	}
	VisibilityType v = static_cast<VisibilityType>(visibility);
	std::string vtext = "?";
	if (v == Chat::CV_CRYPT) {
		std::string decrypted_message;
		std::string key = GMI().GetConfig().client_chat_crypt_key.Get();
		std::vector<char> cipher_data(message.begin(), message.end());
		CryptoError err = CryptoDecryptText(key, cipher_data, decrypted_message);
		if (err == CryptoError::CE_NO_ERROR) {
			message = decrypted_message;
		} else {
			Output::Warning("CRYPT: Decrypt failed. err = {}", CryptoErrString(err));
			message = "<unencrypted data>";
		}
	}
	auto it = Chat::VisibilityNames.find(v);
	if (it != Chat::VisibilityNames.end())
		vtext = it->second;
	std::time_t t = std::time(nullptr);
	std::string time = Utils::FormatDate(std::localtime(&t), "%H:%M:%S");
	std::string room = std::to_string(room_id);
	AddLogEntry(ChatLogText{
		{"<", tcfg.color_log_divider},
		{name, tcfg.color_log_name},
		{"> ", tcfg.color_log_divider},
		{vtext, tcfg.color_log_visibility},
		{" #" + room, tcfg.color_log_room}
	}, v, sys_name);
	AddLogEntry(ChatLogText{
		{message, tcfg.color_log_message},
		{" \uFF00[", tcfg.color_log_divider},
		{time, tcfg.color_log_time},
		{"]", tcfg.color_log_divider}
	}, v, "");
	AddNotificationLogEntry(ChatLogText{
		{"<", tcfg.color_log_divider},
		{name, tcfg.color_log_name},
		{"> ", tcfg.color_log_divider},
		{message, tcfg.color_log_message}
	}, v, sys_name);
	time = Utils::FormatDate(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
	Output::InfoNoChat("[{}] Chat: {} [{}, {}]: {}", time, name, vtext, room_id, message);
}

void ChatUi::GotSystemMessage(std::string message, int visibility) {
	if (chat_box == nullptr) return;
	if (visibility != 0) {
		VisibilityType v = static_cast<VisibilityType>(visibility);
		PrintD(message, true, v);
		return;
	// Ignore messages from the local Player-hosted server
	} else if (message.size() > 5 && std::string_view(message.data(), 5) == "I: S:") {
		PrintD(message, true, Chat::CV_VERBOSE);
		return;
	}
	PrintD(message, true);
}

void ChatUi::SetTextConfig(ChatUiTextConfig tcfg) {
	::tcfg = tcfg;
}

void ChatUi::SetStatusConnection(bool connected, bool connecting) {
	if (chat_box == nullptr)
		return;
	chat_box->SetStatusConnection(connected, connecting);
	if (connected)
		SendKeyHash();
}

void ChatUi::SetStatusRoom(unsigned int room_id) {
	if (chat_box == nullptr)
		return;
	chat_box->SetStatusRoom(room_id);
}

void ChatUi::SetStatusProgress(unsigned int percent, std::string text) {
	if (chat_box == nullptr)
		return;
	chat_box->SetStatusProgress(percent);
	dto_downloading_text = std::move(text);
}

bool ChatUi::IsCheating() const {
	return cheat_flag;
}
