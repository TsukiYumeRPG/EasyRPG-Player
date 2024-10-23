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

#include "packet.h"
#include <sstream>

#ifndef SERVER
#  include "util/crypto.h"
#endif

using namespace Multiplayer;

std::string Packet::ToBytes(std::string_view crypt_key) const {
	std::ostringstream oss(std::ios_base::binary);
	WritePartial(oss, (uint16_t)0x2828); // just to see boundaries from hexdump
	WritePartial(oss, GetType());
	Serialize(oss);

	if (Encrypted()) {
		WritePartial(oss, (uint8_t)true); // encrypted
		WritePartial(oss, packet_crypt);
	} else {
		std::ostringstream oss2(std::ios_base::binary);
		Serialize2(oss2);
		std::string out2 = oss2.str();
		if (out2.size() > 0) {
			if (crypt_key.empty()) {
				WritePartial(oss, (uint8_t)false); // not encrypted
				WritePartial(oss, out2);
			} else {
#ifndef SERVER
				WritePartial(oss, (uint8_t)true);
				std::vector<char> cipher_data;
				CryptoError err = CryptoEncryptText(crypt_key, out2, cipher_data);
				if (err == CryptoError::CE_NO_ERROR) {
					WritePartial(oss, std::string_view(cipher_data.data(), cipher_data.size()));
				}
#endif
			}
		} else {
			WritePartial(oss, (uint8_t)false);
		}
	}

	return SerializeString16(oss.str());
}

void Packet::FromStream(std::istream& is, std::string_view crypt_key) {
	DeSerialize(is);

	if (ReadU8(is)) { // encrypted
		if (crypt_key.empty()) {
			packet_crypt = DeSerializeString16(is);
		} else {
#ifndef SERVER
			std::string decrypted_data;
			std::string encrypted_data = DeSerializeString16(is);
			std::vector<char> cipher_data(encrypted_data.begin(), encrypted_data.end());
			CryptoError err = CryptoDecryptText(crypt_key, cipher_data, decrypted_data);
			if (err == CryptoError::CE_NO_ERROR) {
				std::istringstream iss2(decrypted_data, std::ios_base::binary);
				DeSerialize2(iss2);
			}
#endif
		}
	} else {
		std::istringstream iss2(DeSerializeString16(is), std::ios_base::binary);
		DeSerialize2(iss2);
	}
}
