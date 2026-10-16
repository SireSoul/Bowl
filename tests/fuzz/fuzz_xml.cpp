// XML, as Tiled writes .tmx maps and .tsx tilesets.

#include "fuzz.hpp"

#include "bowl/core/xml.hpp"

using namespace bowl;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    xml::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
