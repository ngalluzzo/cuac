#include <curl/curl.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string JsonString(const char *value) {
	if (value == NULL) {
		throw std::runtime_error("libcurl identity contains a null string");
	}
	std::ostringstream output;
	output << '"';
	for (const unsigned char character : std::string(value)) {
		switch (character) {
		case '\\':
			output << "\\\\";
			break;
		case '"':
			output << "\\\"";
			break;
		case '\b':
			output << "\\b";
			break;
		case '\f':
			output << "\\f";
			break;
		case '\n':
			output << "\\n";
			break;
		case '\r':
			output << "\\r";
			break;
		case '\t':
			output << "\\t";
			break;
		default:
			if (character < 0x20) {
				output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(character)
				       << std::dec;
			} else {
				output << character;
			}
		}
	}
	output << '"';
	return output.str();
}

std::string VersionNumber(unsigned int value) {
	std::ostringstream output;
	output << "0x" << std::hex << std::setw(6) << std::setfill('0') << value;
	return output.str();
}

class CurlGlobalCleanup {
public:
	~CurlGlobalCleanup() {
		curl_global_cleanup();
	}
};

} // namespace

int main() {
	try {
		std::string record;
		const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (initialized != CURLE_OK) {
			throw std::runtime_error("curl_global_init failed");
		}
		{
			const CurlGlobalCleanup cleanup;
			const curl_version_info_data *identity = curl_version_info(CURLVERSION_NOW);
			if (identity == NULL) {
				throw std::runtime_error("curl_version_info returned null");
			}
			std::ostringstream output;
			output << "{\"features\":" << identity->features << ",\"ssl_version\":" << JsonString(identity->ssl_version)
			       << ",\"version\":" << JsonString(identity->version)
			       << ",\"version_num\":" << JsonString(VersionNumber(identity->version_num).c_str()) << "}";
			record = output.str();
		}
		std::cout << record << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "native dependency identity failed: " << error.what() << std::endl;
		return 1;
	}
}
