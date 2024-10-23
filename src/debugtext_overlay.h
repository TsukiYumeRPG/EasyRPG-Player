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

#ifndef EP_DEBUGTEXT_OVERLAY_H
#define EP_DEBUGTEXT_OVERLAY_H

#include <map>
#include <vector>
#include <string>
#include "drawable.h"
#include "rect.h"
#include "memory_management.h"

struct DebugTextOverlayItem {
	bool show = false;
	bool dirty = false;
	bool remove = false;
	Rect rect;
	std::vector<std::string> lines;
};

class DebugTextOverlay : public Drawable {
public:
	DebugTextOverlay();

	void Draw(Bitmap& dst) override;

	void UpdateItem(const std::string& name, const std::string& debugtext);
	void ShowItem(const std::string& name);
	void HideItem(const std::string& name);
	void RemoveItem(const std::string& name);

	void SetColor(int color);

private:
	Rect bitmap_rect;
	BitmapRef bitmap;

	std::map<std::string, DebugTextOverlayItem> items;
	std::vector<std::string> remove_list;

	int color = 0;
};

#endif
