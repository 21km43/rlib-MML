#include <stdio.h>
#include <emscripten.h>
#include <emscripten/bind.h>

#include <iostream>
#include <memory>
#include <vector>
#include <sstream>
#include <regex>
#include <exception>

#include "../stringformat/StringFormat.h"
#include "../sequencer/MmlToSmf.h"
#include "../sequencer/SmfToMml.h"

emscripten::val mmlToSmf(const std::string& mml) {
	auto ret = emscripten::val::object();
	try {
		// std::cout << "mmlToSmf" << std::endl;
		const auto r = rlib::sequencer::mmlToSmf(mml);
		if (r.hasError()) {
			const auto errorJson = rlib::sequencer::MmlCompiler::Result::getJson(mml, r.errors);
			ret.set("error", errorJson);
		} else {
			const auto v = r.smf.getFileImage();
			const auto uint8Array = emscripten::val::global("Uint8Array").new_(v.size());
			uint8Array.call<void>("set", emscripten::typed_memory_view(v.size(), v.data()));	// wasm側のメモリをコピー
			ret.set("result", uint8Array);
		}
	} catch (const std::exception& e) {
		// std::cout << "mmlToSmf std::exception" << std::endl;
		ret.set("error", emscripten::val::u8string(e.what()));
	} catch (...) {
		// std::cout << "mmlToSmf unknown exception" << std::endl;
		ret.set("error", emscripten::val::u8string("unknown exception"));
	}
	return ret;
}



emscripten::val smfToMml(const std::string& smfBinary) {
	auto ret = emscripten::val::object();
	try {
		// std::cout << "smfToMml" << std::endl;
		std::istringstream is(smfBinary, std::istringstream::binary);
		auto smf = rlib::midi::Smf::fromStream(is);
		const auto mml = rlib::sequencer::smfToMml(smf);
		ret.set("result", emscripten::val::u8string(mml.c_str()));
		ret.set("ok", true);
	} catch (std::exception& e) {
		// std::cout << "smfToMml std::exception" << std::endl;
		ret.set("message", emscripten::val::u8string(e.what()));
		ret.set("ok", false);
	} catch (...) {
		// std::cout << "smfToMml unknown exception" << std::endl;
		ret.set("message", emscripten::val::u8string("unknown exception"));
		ret.set("ok", false);
	}
	return ret;
}

EMSCRIPTEN_BINDINGS(module) {
	emscripten::function("mmlToSmf", &mmlToSmf);
	emscripten::function("smfToMml", &smfToMml);
}
