#pragma once

#include "Utils/D3D.h"
#include "Utils/Format.h"

#include <vector>
#include <winrt/base.h>

namespace Util
{
	/**
	 * @brief Synchronous, compile-once cache for a single standalone shader.
	 *
	 * Get() compiles on first call and caches the result, including failure --
	 * a failed compile returns nullptr on every call until Reset() is called.
	 * Callers must null-check Get()'s return before binding it, since
	 * Util::CompileShader returns nullptr on any compile or shader-object
	 * creation failure. Reset() belongs in the owning feature's
	 * ClearShaderCache(), the same entry point ShaderCache's file watcher
	 * already calls to invalidate standalone (non-BSShader) shaders.
	 */
	template <typename ShaderT>
	class LazyShader
	{
	public:
		/**
		 * @brief Returns the cached shader, compiling it on first call. a_name, if given, is passed to
		 * Util::SetResourceName on a successful first compile for RenderDoc debuggability.
		 */
		ShaderT* Get(const wchar_t* a_path, const std::vector<std::pair<const char*, const char*>>& a_defines, const char* a_target, const char* a_entry = "main", const char* a_name = nullptr)
		{
			if (!shader && !failed) {
				logger::debug("Compiling {}", Util::WStringToString(a_path));
				if constexpr (std::is_same_v<ShaderT, ID3DBlob>) {
					shader = Util::CompileShaderBlob(a_path, a_defines, a_target, a_entry);
				} else {
					shader.attach(static_cast<ShaderT*>(Util::CompileShader(a_path, a_defines, a_target, a_entry)));
				}
				failed = !shader;
				if constexpr (!std::is_same_v<ShaderT, ID3DBlob>) {
					if (shader && a_name)
						Util::SetResourceName(shader.get(), a_name);
				}
			}
			return shader.get();
		}

		/** @brief Drops the cached shader (and any cached failure), forcing recompilation on the next Get(). */
		void Reset()
		{
			shader = nullptr;
			failed = false;
		}

		/** @brief Returns the cached shader without attempting to compile -- for a call site that already knows Get() ran elsewhere this frame. */
		ShaderT* get() const { return shader.get(); }

		/** @brief True if a shader is currently cached (i.e. the last Get() succeeded). */
		explicit operator bool() const { return shader != nullptr; }

	private:
		winrt::com_ptr<ShaderT> shader;
		bool failed = false;
	};
}
