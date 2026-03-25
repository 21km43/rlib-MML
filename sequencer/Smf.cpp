
//#ifndef _MSC_VER
//#include <bits/stdc++.h>
//#endif

#include <array>
#include <cstdint>
#include <istream>
#include <iostream>
#include <math.h>
#include <sstream>
#include <format>

#include "./Smf.h"

using namespace rlib;
using namespace rlib::midi;

namespace {
#pragma pack( push )
#pragma pack( 1 )

	struct HeaderChunk {
		std::array<uint8_t, 4>	MThd = { 'M', 'T', 'h', 'd' };	// MThd
		uint32_t				dataLength = 6;					// データ長(6固定)
		uint16_t				format = 0;						// フォーマット
		uint16_t				trackCount = 0;					// トラック数
		uint16_t				division = 0;					// 時間単位
	};

	struct TrackChunk {
		std::array<uint8_t, 4>	MTrk = { 'M','T','r','k' };	// MTrk
		uint32_t				dataLength = 0;				// データ長
	};

#pragma pack( pop )

	// エンディアン変更
	template <typename T> static void changeEndian(T& t) {
		std::reverse(reinterpret_cast<uint8_t*>(&t), reinterpret_cast<uint8_t*>(&t) + sizeof(t));
	}

	class Read {
		size_t m_readBytes = 0;
	public:
		std::istream& is;
		Read(std::istream& is_)
			:is(is_) {
		}
		const size_t readBytes()const {
			return m_readBytes;
		}
		template <typename T> T read() {
			typename std::remove_const<T>::type buf;
			if (is.read(reinterpret_cast<char*>(&buf), sizeof(buf)).gcount() < sizeof(buf)) {
				throw std::runtime_error("size error");
			}
			m_readBytes += sizeof(buf);
			return buf;
		}

		std::vector<uint8_t> read(size_t bytes) {
			std::vector<uint8_t> buf(bytes);
			if (bytes > 0) {
				const auto readSize = is.read(reinterpret_cast<char*>(buf.data()), buf.size()).gcount();
				m_readBytes += readSize;
				buf.resize(readSize);
			}
			return buf;
		}

		void unget() {
			if (!is.unget()) throw std::runtime_error("unget error");	// 1Byte戻す
			m_readBytes--;
		}

	};
};


std::vector<uint8_t> Smf::getFileImage() const
{
	std::list<std::vector<uint8_t>> trackData;

	for (auto& track : tracks) {
		std::vector<uint8_t> v;

		size_t position = 0;		// 現在位置

		auto fEvent = [&](const Event& event) {

			{// DeltaTime
				assert(event.position >= position);
				const size_t deltaTime = event.position - position;		// DeltaTime
				position = event.position;								// 現在位置更新
				std::vector<uint8_t> s = midi::utility::getVariableValue(deltaTime);
				v.insert(v.end(), std::make_move_iterator(s.begin()), std::make_move_iterator(s.end()));
			}

			{
				std::vector<uint8_t> t = event.event->smfBytes();
				v.insert(v.end(), std::make_move_iterator(t.begin()), std::make_move_iterator(t.end()));
			}

		};

		for (auto& event : track.events) {
			fEvent(event);
		}

		// EndOfTrackがなければ付ける
		[&] {
			if (auto i = track.events.rbegin(); i != track.events.rend()) {		// 末尾が EndOfTrack ではないなら
				if (auto meta = std::dynamic_pointer_cast<const midi::EventMeta>(i->event)) {
					if (meta->type == midi::EventMeta::Type::endOfTrack) {
						return;
					}
				}
			}
			Event e(position, std::make_shared<midi::EventMeta>(midi::EventMeta::createEndOfTrack()));
			fEvent(e);
		}();

		{// TrackChunk を先頭に挿入
			TrackChunk trackChunk;
			trackChunk.dataLength = static_cast<uint32_t>(v.size());
			changeEndian(trackChunk.dataLength);
			v.insert(v.begin(),
				reinterpret_cast<const uint8_t*>(&trackChunk),
				reinterpret_cast<const uint8_t*>(&trackChunk) + sizeof(trackChunk));
		}

		trackData.emplace_back(std::move(v));
	}

	const HeaderChunk headerChunk = [&] {
		HeaderChunk h;
		h.trackCount = static_cast<uint16_t>(trackData.size());
		h.format = h.trackCount > 1 ? 1 : 0;
		h.division = timeBase;
		// エンディアン変更
		changeEndian(h.dataLength);
		changeEndian(h.format);
		changeEndian(h.trackCount);
		changeEndian(h.division);
		return h;
	}();

	std::vector<uint8_t> result(
		reinterpret_cast<const uint8_t*>(&headerChunk),
		reinterpret_cast<const uint8_t*>(&headerChunk) + sizeof(headerChunk));

	for (auto& i : trackData) {
		result.insert(result.end(), std::make_move_iterator(i.begin()), std::make_move_iterator(i.end()));
	}

	return result;
}

Smf Smf::fromStream(std::istream& is) {
	using namespace midi;

	Smf smf;
	Read file(is);

	const auto headerChunk = [&file]() {
		HeaderChunk c = file.read<decltype(c)>();
		if (c.MThd != HeaderChunk().MThd) {
			throw std::runtime_error("MThd chunk error");
		}
		changeEndian(c.dataLength);
		changeEndian(c.format);
		changeEndian(c.trackCount);
		changeEndian(c.division);
		return c;
	}();

	smf.timeBase = headerChunk.division;

	// トラックチャンク
	std::exception_ptr ep;	// 例外保持
	try {
		for (size_t i = 0; i < headerChunk.trackCount; i++) {
			const auto trackChunk = [&file]() {
				TrackChunk c = file.read<decltype(c)>();
				if (c.MTrk != TrackChunk().MTrk) {
					throw std::runtime_error("MTrk chunk error.");
				}
				changeEndian(c.dataLength);
				return c;
			}();

			const auto trackBinary = [&]() {
				auto v = file.read(trackChunk.dataLength);
				if (v.size() < trackChunk.dataLength) std::clog << "[warning] track data size error." << std::endl;	// データが足りない(が、エラーにはせず続行)
				return std::string(v.begin(), v.end());
			}();
			std::istringstream iss(trackBinary);
			Read tr(iss);

			Smf::Track track;
			try {
				std::uint64_t currentPosition = 0;		// 現在位置

				struct {
					uint8_t before = 0;	// 直前のステータス(0x80～0xef)
					uint8_t beforeF = 0;	// 直前のステータス(0x80～0xff) 違反チェック用
				}runningStatus;

				while (tr.readBytes() < trackBinary.size()) {
					auto readVariableValue = [&tr] {					// 可変長数値を取得
						return utility::readVariableValue([&] {return tr.read<uint8_t>(); });
					};

					currentPosition += readVariableValue();		// 現在位置 += デルタタイム

					// status 読み込み
					const auto status = [&] {
						const uint8_t status = tr.read<decltype(status)>();
						if (!(status & 0x80)) {					// status 省略なら直前値を採用
							tr.unget();							// 1Byte戻す
							if (runningStatus.beforeF >= 0xf0) {	// 仕様違反チェック
								std::clog << "[warning] running status transition after SysEx/Meta. recovery with previous status " << std::format("0x{:02x}", runningStatus.before & 0xf0) << std::endl;
								runningStatus.beforeF = runningStatus.before;
								return runningStatus.before;
							}
							return runningStatus.before;
						}
						runningStatus.beforeF = status;
						if (status < 0xf0) runningStatus.before = status;
						return status;
					}();

					switch (status & 0xf0) {
					case EventNoteOff::statusByte: {
						const std::array<uint8_t, 2> a = tr.read<decltype(a)>();
						track.events.emplace(currentPosition, std::make_shared<EventNoteOff>(status & 0xf, a[0] & 0x7f, a[1] & 0x7f));
						break;
					}
					case EventNoteOn::statusByte: {
						const std::array<uint8_t, 2> a = tr.read<decltype(a)>();
						track.events.emplace(currentPosition, std::make_shared<EventNoteOn>(status & 0xf, a[0] & 0x7f, a[1] & 0x7f));
						break;
					}
					case EventPolyphonicKeyPressure::statusByte: {
						const std::array<uint8_t, 2> a = tr.read<decltype(a)>();
						track.events.emplace(currentPosition, std::make_shared<EventPolyphonicKeyPressure>(status & 0xf, a[0] & 0x7f, a[1] & 0x7f));
						break;
					}
					case EventControlChange::statusByte: {
						const std::array<uint8_t, 2> a = tr.read<decltype(a)>();
						track.events.emplace(currentPosition, std::make_shared<EventControlChange>(status & 0xf, a[0] & 0x7f, a[1] & 0x7f));
						break;
					}
					case EventProgramChange::statusByte: {
						const uint8_t n = tr.read<decltype(n)>();
						track.events.emplace(currentPosition, std::make_shared<EventProgramChange>(status & 0xf, n & 0x7f));
						break;
					}
					case EventPitchBend::statusByte: {
						const std::array<uint8_t, 2> a = tr.read<decltype(a)>();
						const auto n = ((a[0] & 0x7f) + (a[1] & 0x7f) * 0x80) - 8192;
						track.events.emplace(currentPosition, std::make_shared<EventPitchBend>(status & 0xf, n));
						break;
					}
					case EventChannelPressure::statusByte: {
						const uint8_t n = tr.read<decltype(n)>();
						track.events.emplace(currentPosition, std::make_shared<EventChannelPressure>(status & 0xf, n & 0x7f));
						break;
					}
					default:
						switch (status) {
						case EventSystemExclusive::statusByteF0:
						case EventSystemExclusive::statusByteF7: {
							const auto len = readVariableValue();		// データ長
							std::vector<uint8_t> v{ status };
							const auto data = tr.read(len);
							if (data.size() < len) std::clog << "[warning] system exclusive data size error." << std::endl;	// データが足りない(が、エラーにはせず続行)
							v.insert(v.end(), data.begin(), data.end());
							track.events.emplace(currentPosition, std::make_shared<EventSystemExclusive>(std::move(v)));
							break;
						}
						case EventMeta::statusByte: {
							const uint8_t type = tr.read<decltype(type)>();						// イベントタイプ
							const auto len = readVariableValue();		// データ長
							auto data = tr.read(len);
							if (data.size() < len) std::clog << "[warning] meta data size error." << std::endl;	// データが足りない(が、エラーにはせず続行)
							const auto spEvent = std::make_shared<EventMeta>(static_cast<EventMeta::Type>(type), std::move(data));
							track.events.emplace(currentPosition, spEvent);

							if (spEvent->type == EventMeta::Type::endOfTrack) { // End of Track が現れたら当該トラックの残りデータは無視
								tr.read(trackChunk.dataLength - tr.readBytes());
							}
							break;
						}

						case 0xf1:	// MIDI Time Code Quarter Frame
						case 0xf2:	// Song Position Pointer
						case 0xf3:	// Song Select
						case 0xf4:	// Reserved
						case 0xf5:	// Reserved
						case 0xf6:	// Tune Request
						// case 0xf7:	// End of Exclusive
						case 0xf8:	// Timing Clock
						case 0xf9:	// Reserved
						case 0xfa:	// Start
						case 0xfb:	// Continue
						case 0xfc:	// Stop
						case 0xfd:	// Reserved
						case 0xfe:	// Active Sense
						// case 0xff:	// System Reset
							break;		// これらは無視する。1バイトイベントとして次へ。
						default:
							throw std::runtime_error("unknown status byte");
							break;
						}
					}
				}
			} catch (...) {
				if (track.events.size() > 0) {
					smf.tracks.emplace_back(std::move(track));
				}
				throw;
			}
			smf.tracks.emplace_back(std::move(track));
		}
	} catch (...) {
		ep = std::current_exception();
	}
	if (ep) {
		try {
			std::rethrow_exception(ep);
		} catch (const std::exception& e) {
			std::clog << "[warning] exception: " << e.what() << std::endl;
		} catch (...) {
			std::clog << "[warning] exception unknwon" << std::endl;
		}
		if (smf.tracks.size() <= 0) {	// トラックがまったくパースできてない状態なら
			std::rethrow_exception(ep);	// throw で終了
		}
	}
	return smf;
}

Smf Smf::convertTimebase(const Smf& smf, int timeBase) {
	Smf dst;
	dst.timeBase = timeBase;
	const auto mul = static_cast<double>(dst.timeBase) / smf.timeBase;
	for (auto& track : smf.tracks) {
		Track dstTrack;
		for (auto& event : track.events) {
			const auto dstPos = static_cast<decltype(Event::position)>(std::round(event.position * mul));
			dstTrack.events.emplace(dstPos, event.event);
		}
		dst.tracks.push_back(dstTrack);
	}
	return dst;
}
