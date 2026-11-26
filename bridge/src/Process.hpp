// Small cross-platform helpers for running tracy-capture and the bridge itself.
#pragma once

#include <string>
#include <vector>

namespace tlb
{

struct ChildProcess
{
#ifdef _WIN32
    void* handle = nullptr;
#else
    int pid = -1;
#endif
    bool Valid() const;
};

// Starts `exe` with `args`. quietStdout sends the child's stdout to the null device
// (stderr stays on our console, so its errors still show).
bool SpawnProcess( const std::string& exe, const std::vector<std::string>& args, bool quietStdout,
                   ChildProcess& child, std::string& error );
// Exit code, or -1 if it is still running after timeoutMs.
int WaitProcess( ChildProcess& child, int timeoutMs );
void KillProcess( ChildProcess& child );

// Full path of this executable.
std::string SelfPath();
// A program: the explicit path if given, else `name` (plus .exe on Windows) next to
// this executable, else on PATH. Empty if not found.
std::string FindTool( const std::string& explicitPath, const std::string& name );
// FindTool for tracy-capture.
std::string FindCaptureTool( const std::string& explicitPath );
// A TCP port on 127.0.0.1 that was free a moment ago.
int FreeLocalPort();
void SetEnvVar( const char* name, const std::string& value );

}  // namespace tlb
