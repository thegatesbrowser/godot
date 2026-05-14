// the_gates: catch-all stub for chromium 137 symbols the sandbox lib calls
// transitively but our wrapper never exercises. Each stub uses the real
// chromium headers so signatures stay in lockstep with whatever the .cc that
// references them sees.

#include <windows.h>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/memory/protected_memory.h"
#include "base/path_service.h"
#include "base/task/current_thread.h"
#include "base/threading/hang_watcher.h"

namespace base {

// ----- FilePath helpers chromium 137 added that Firefox's shim doesn't ----

bool FilePath::IsAbsolute() const {
  const StringType& v = value();
  if (v.size() >= 2 && v[1] == L':' &&
      ((v[0] >= L'A' && v[0] <= L'Z') || (v[0] >= L'a' && v[0] <= L'z'))) {
    return true;
  }
  return v.size() >= 2 && v[0] == L'\\' && v[1] == L'\\';
}

bool FilePath::operator==(const FilePath& other) const {
  return value() == other.value();
}

// ----- GetPageSize -----

size_t GetPageSize() {
  SYSTEM_INFO info{};
  ::GetSystemInfo(&info);
  return info.dwPageSize ? info.dwPageSize : 4096u;
}

// ----- CommandLine — no-op so time_win.cc's MaybeAddHighResolutionTimeTicks
//       check returns false and nothing else happens. We don't store any
//       state.

// We never make InitializedForCurrentProcess() return true, so ForCurrentProcess
// is dead code. Return nullptr — saves us from having to drag in the chromium
// command_line.cc constructor + storage. The only call site in our build
// (time_win.cc:614) gates on InitializedForCurrentProcess first.
CommandLine* CommandLine::ForCurrentProcess() { return nullptr; }
bool CommandLine::InitializedForCurrentProcess() { return false; }

bool CommandLine::HasSwitch(const char* const /*sw*/) const { return false; }

std::string CommandLine::GetSwitchValueASCII(std::string_view /*sw*/) const {
  return std::string();
}

void CommandLine::AppendSwitchASCII(std::string_view, std::string_view) {}

// ----- PathService — fail every lookup; we never use it -----
bool PathService::Get(int /*key*/, FilePath* /*path*/) { return false; }

// ----- HangWatcher — sandbox never trips it; no-op -----
void HangWatcher::InvalidateActiveExpectations() {}

// ----- CurrentIOThread / CurrentUIThread — sandbox is its own thread -----
bool CurrentIOThread::IsSet() { return false; }
bool CurrentUIThread::IsSet() { return false; }

// ----- AutoWritableMemory — code-page protection toggles. Interceptors call
// these when patching ntdll thunks. SetMemoryReadWrite/ReadOnly used to be
// inline in the header in older chromium; 137 moved them to .cc. Provide
// safe defaults: actually flip page protection.
bool AutoWritableMemoryBase::SetMemoryReadOnly(void* start, void* end) {
  if (start == nullptr || end == nullptr || end <= start) return true;
  DWORD old = 0;
  return ::VirtualProtect(start,
                          static_cast<SIZE_T>(static_cast<char*>(end) -
                                              static_cast<char*>(start)),
                          PAGE_EXECUTE_READ, &old) != FALSE;
}
bool AutoWritableMemoryBase::SetMemoryReadWrite(void* start, void* end) {
  if (start == nullptr || end == nullptr || end <= start) return true;
  DWORD old = 0;
  return ::VirtualProtect(start,
                          static_cast<SIZE_T>(static_cast<char*>(end) -
                                              static_cast<char*>(start)),
                          PAGE_EXECUTE_READWRITE, &old) != FALSE;
}

namespace internal {
void CheckMemoryReadOnly(const void* /*p*/) {}
}  // namespace internal

}  // namespace base

// ----- switches:: constants chromium command_line callers reference -----
namespace switches {
extern const char kForceHighResTimeTicks[];
const char kForceHighResTimeTicks[] = "force-high-res-time-ticks";
}

// PR_ParseTimeString + double_conversion now provided by their real .cc
// files (vendored under base/third_party/{nspr,double_conversion}/), no
// stubs needed.
