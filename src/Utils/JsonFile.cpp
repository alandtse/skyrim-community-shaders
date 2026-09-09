#include "Utils/FileSystem.h"

#include <Windows.h>
#include <fstream>

namespace Util::FileHelpers
{
	bool WriteJsonAtomically(const std::filesystem::path& path, const json& data, int indent,
		std::string_view context)
	{
		std::string serialized;
		try {
			serialized = data.dump(indent);
		} catch (const std::exception& e) {
			logger::error("[Settings] Could not serialize {} '{}': {}", context, path.string(), e.what());
			return false;
		}

		std::error_code ec;
		if (!path.parent_path().empty()) {
			std::filesystem::create_directories(path.parent_path(), ec);
			if (ec) {
				logger::error("[Settings] Could not create directory for {} '{}': {}",
					context, path.string(), ec.message());
				return false;
			}
		}

		auto temporaryPath = path;
		temporaryPath += std::format(".{}.tmp", ::GetCurrentProcessId());
		{
			std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				logger::error("[Settings] Could not open temporary {} file '{}'", context, temporaryPath.string());
				return false;
			}
			file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
			file.flush();
			if (file.fail()) {
				logger::error("[Settings] Could not write temporary {} file '{}'", context, temporaryPath.string());
				file.close();
				std::filesystem::remove(temporaryPath, ec);
				return false;
			}
			file.close();
			if (file.fail()) {
				logger::error("[Settings] Could not close temporary {} file '{}'", context, temporaryPath.string());
				std::filesystem::remove(temporaryPath, ec);
				return false;
			}
		}

		if (!::MoveFileExW(temporaryPath.c_str(), path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			const auto error = ::GetLastError();
			logger::error("[Settings] Could not replace {} '{}' (Win32 error {})",
				context, path.string(), error);
			std::filesystem::remove(temporaryPath, ec);
			return false;
		}
		return true;
	}

	std::filesystem::path ResolveExistingFile(const std::filesystem::path& path)
	{
		const auto file = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return {};
		const SKSE::stl::scope_exit closeFile([file] { ::CloseHandle(file); });
		const auto length = ::GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED);
		if (length == 0)
			return {};
		std::wstring resolved(length, L'\0');
		const auto written = ::GetFinalPathNameByHandleW(file, resolved.data(), length, FILE_NAME_NORMALIZED);
		if (written == 0 || written >= length)
			return {};
		resolved.resize(written);
		return resolved;
	}
}
