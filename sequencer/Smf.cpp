
//#ifndef _MSC_VER
//#include <bits/stdc++.h>
//#endif

#include <array>
#include <cstdint>
#include <istream>
#include <iostream>
#include <math.h>

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
			const auto readed = is.read(reinterpret_cast<char*>(buf.data()), buf.size()).gcount();
			m_readBytes += readed;
			buf.resize(readed);
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
				std::vector<uint8_t> s = midi::inner::getVariableValue(deltaTime);
				v.insert(v.end(), std::make_move_iterator(s.begin()), std::make_move_iterator(s.end()));
			}

			{
				std::vector<uint8_t> t = event.event->midiMessage();
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

Smf Smf::fromStream(std::istream& is)
{
	using namespace midi;

	Smf smf;
	Read r(is);

	const auto headerChunk = [&r]() {
		HeaderChunk c = r.read<decltype(c)>();
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
			const auto trackChunk = [&r]() {
				TrackChunk c = r.read<decltype(c)>();
				if (c.MTrk != TrackChunk().MTrk) {
					throw std::runtime_error("MTrk chunk error.");
				}
				changeEndian(c.dataLength);
				return c;
			}();

			Smf::Track track;
			try{
				std::uint64_t currentPosition = 0;		// 現在位置
				uint8_t beforeStatus = 0;
				for (const auto beginPosition = r.readBytes(); r.readBytes() < beginPosition + trackChunk.dataLength; ) {
					auto readVariableValue = [&r] {					// 可変長数値を取得
						return inner::readVariableValue([&] {return r.read<uint8_t>(); });
					};

					currentPosition += readVariableValue();		// 現在位置 += デルタタイム

					// status 読み込み
					const auto status = [&] {
						const uint8_t status = r.read<decltype(status)>();
						if (!(status & 0x80)) {					// status 省略なら直前値を採用
							r.unget();							// 1Byte戻す
							return beforeStatus;
						}
						beforeStatus = status;
						return status;
					}();

					switch (status & 0xf0) {
					case EventNoteOff::statusByte: {
						const std::array<uint8_t, 2> a = r.read<decltype(a)>();
						track.events.emplace(currentPosition, std::make_shared<EventNoteOff>(status & 0xf, a[0] & 0x7f, a[1] & 0x7f));
						break;
					}
					case EventNoteOn::statusByte: {
						const std::array<uint8_t, 2> a = r.read<decltype(a)>();
						track.events.emplace(currentPosition, std::make_shared<EventNoteOn>(status & 0xf, a[0] & 0x7f, a[1] & 0x7f));
						break;
					}
					case EventPolyphonicKeyPressure::statusByte: {
						const std::array<uint8_t, 2> a = r.read<decltype(a)>();
						track.events.emplace(currentPosition, std::make_shared<EventPolyphonicKeyPressure>(status & 0xf, a[0] & 0x7f, a[1] & 0x7f));
						break;
					}
					case EventControlChange::statusByte: {
						const std::array<uint8_t, 2> a = r.read<decltype(a)>();
						track.events.emplace(currentPosition, std::make_shared<EventControlChange>(status & 0xf, a[0] & 0x7f, a[1] & 0x7f));
						break;
					}
					case EventProgramChange::statusByte: {
						const uint8_t n = r.read<decltype(n)>();
						track.events.emplace(currentPosition, std::make_shared<EventProgramChange>(status & 0xf, n & 0x7f));
						break;
					}
					case EventPitchBend::statusByte: {
						const std::array<uint8_t, 2> a = r.read<decltype(a)>();
						const auto n = ((a[0] & 0x7f) + (a[1] & 0x7f) * 0x80) - 8192;
						track.events.emplace(currentPosition, std::make_shared<EventPitchBend>(status & 0xf, n));
						break;
					}
					case EventChannelPressure::statusByte: {
						const uint8_t n = r.read<decltype(n)>();
						track.events.emplace(currentPosition, std::make_shared<EventChannelPressure>(status & 0xf, n & 0x7f));
						break;
					}
					default:
						switch (status) {
						case EventExclusive::statusByte: {
							const auto size = readVariableValue();		// データ長
							std::vector<uint8_t> data;
							for (auto i = 0; i < size; i++) {
								const uint8_t n = r.read<decltype(n)>();
								if (n == 0xf7) {				// EOX(終了コード)
									if (i != size - 1) {
										// ヘンなところで終わった(が、エラーにはせず続行)
										std::clog << "[warning] exclusive size error. EOX(0xf7) is failed position." << std::endl;	// throw std::runtime_error("exclusive size error");
										break;
									}
									break;
								}
								if ((n & 0x80) != 0) {
									// あるべきハズのEOX(0xf7)が無い(が、エラーにはせず続行)
									std::clog << "[warning] exclusive data error. EOX(0xf7) is missing." << std::endl;
									break;
								}
								data.push_back(n);
							}
							track.events.emplace(currentPosition, std::make_shared<EventExclusive>(std::move(data)));
							break;
						}
						case EventMeta::statusByte: {
							const uint8_t type = r.read<decltype(type)>();						// イベントタイプ
							const auto len = readVariableValue();								// データ長
							auto data = r.read(len);
							if (data.size() < len) {
								std::clog << "[warning] meta data size error." << std::endl;	// データが足りない(が、エラーにはせず続行)
							}
							track.events.emplace(currentPosition, std::make_shared<EventMeta>(static_cast<EventMeta::Type>(type), std::move(data)));
							break;
						}
						default:
							assert(false);
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
