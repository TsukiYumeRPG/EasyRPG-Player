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

#include "debugtext_overlay.h"
#include "bitmap.h"
#include "font.h"
#include "cache.h"
#include "drawable_mgr.h"
#include "player.h"
#include "game_message.h"

DebugTextOverlay::DebugTextOverlay() :
	Drawable(Priority_Overlay + 50, Drawable::Flags::Global)
{
	DrawableMgr::Register(this);
}

void DebugTextOverlay::Draw(Bitmap& dst) {
	if (items.empty() || !bitmap) return;

	int y = 0;
	for (auto& it : items) {
		auto& item = it.second;
		if (item.dirty) {
			for (const std::string& line : item.lines) {
				Rect text_rect = Text::GetSize(*Font::DefaultBitmapFont(), line);
				Rect line_rect(0, y, Player::screen_width, text_rect.height);
				bitmap->ClearRect(line_rect);
				if (item.show) {
					line_rect.width = text_rect.width;
					bitmap->FillRect(line_rect, Color(0, 0, 0, 102));
					if (color > -1)
						Text::Draw(*bitmap, 0, y, *Font::Default(), *Cache::SystemOrBlack(), color, line);
					else
						Text::Draw(*bitmap, 0, y, *Font::DefaultBitmapFont(), Color(255, 255, 255, 255), line);
				}
				y += text_rect.height;
			}

			item.dirty = false;

			if (item.remove) remove_list.emplace_back(it.first);
		} else {
			y += item.rect.height;
		}
	}

	if (!remove_list.empty()) {
		for (const auto& name : remove_list) {
			items.erase(name);
		}
		remove_list.clear();
	}

	dst.Blit(0, 0, *bitmap, bitmap_rect, 192);
}

void DebugTextOverlay::UpdateItem(const std::string& name, const std::string& debugtext) {
	DebugTextOverlayItem& item = items[name];

	item.rect.width = 0;
	item.rect.height = 0;
	item.lines.clear();

	Game_Message::WordWrap(
		debugtext,
		Player::screen_width,
		[&](StringView line) {
			Rect rect_tmp = Text::GetSize(*Font::DefaultBitmapFont(), line);
			if (rect_tmp.width > item.rect.width) {
				item.rect.width = rect_tmp.width;
			}
			item.rect.height += rect_tmp.height;
			item.lines.emplace_back(line);
		},
		*Font::DefaultBitmapFont()
	);

	int width_tmp{0}, height_tmp{0};
	for (const auto& it : items) {
		if(it.second.rect.width > width_tmp) width_tmp = it.second.rect.width;
		height_tmp += it.second.rect.height;
	}
	if (bitmap_rect.width != width_tmp || bitmap_rect.height != height_tmp) {
		bitmap_rect.width = width_tmp;
		bitmap_rect.height = height_tmp;
		bitmap = Bitmap::Create(bitmap_rect.width, bitmap_rect.height);
	}

	if (item.show) item.dirty = true;
}

void DebugTextOverlay::ShowItem(const std::string& name) {
	DebugTextOverlayItem& item = items[name];

	item.show = true;
	item.dirty = true;
}

void DebugTextOverlay::HideItem(const std::string& name) {
	DebugTextOverlayItem& item = items[name];

	item.show = false;
	item.dirty = true;
}

void DebugTextOverlay::RemoveItem(const std::string& name) {
	DebugTextOverlayItem& item = items[name];

	item.show = false;
	item.dirty = true;
	item.remove = true;
}

void DebugTextOverlay::SetColor(int color) {
	this->color = color;
}
