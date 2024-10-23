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

#ifndef EP_CHAT_H
#define EP_CHAT_H

#include <map>
#include <string>

namespace Chat {
	enum VisibilityType : uint8_t {
		CV_NONE = 0,
		CV_VERBOSE = 1,
		CV_LOCAL = 2,
		CV_GLOBAL = 4,
		CV_CRYPT = 8,
		CV_MAX = CV_CRYPT + 1,
	};

	static const std::map<VisibilityType, std::string> VisibilityNames = {
		{ CV_VERBOSE, "VERBOSE" },
		{ CV_LOCAL, "LOCAL" },
		{ CV_GLOBAL, "GLOBAL" },
		{ CV_CRYPT, "CRYPT" },
	};

	static const std::map<std::string, VisibilityType> VisibilityValues = {
		{ "VERBOSE", CV_VERBOSE },
		{ "LOCAL", CV_LOCAL },
		{ "GLOBAL", CV_GLOBAL },
		{ "CRYPT", CV_CRYPT },

		{ "V", CV_VERBOSE },
		{ "L", CV_LOCAL },
		{ "G", CV_GLOBAL },
		{ "C", CV_CRYPT },

		{ "v", CV_VERBOSE },
		{ "l", CV_LOCAL },
		{ "g", CV_GLOBAL },
		{ "c", CV_CRYPT },
	};
}

#endif
