#include "Process.hpp"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/socket.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <chrono>
#  include <thread>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  endif
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace tlb
{

#ifdef _WIN32

bool ChildProcess::Valid() const
{
    return handle != nullptr;
}

namespace
{
// Quotes one argument the way the MSVC runtime splits command lines.
std::string QuoteArg( const std::string& arg )
{
    if( !arg.empty() && arg.find_first_of( " \t\"" ) == std::string::npos ) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for( char c : arg )
    {
        if( c == '\\' )
        {
            backslashes++;
            continue;
        }
        if( c == '"' )
        {
            out.append( backslashes * 2 + 1, '\\' );
        }
        else
        {
            out.append( backslashes, '\\' );
        }
        backslashes = 0;
        out += c;
    }
    out.append( backslashes * 2, '\\' );
    out += '"';
    return out;
}
}  // namespace

bool SpawnProcess( const std::string& exe, const std::vector<std::string>& args, bool quietStdout,
                   ChildProcess& child, std::string& error )
{
    std::string cmd = QuoteArg( exe );
    for( auto& a : args ) cmd += " " + QuoteArg( a );

    STARTUPINFOA si = {};
    si.cb = sizeof( si );
    HANDLE nul = INVALID_HANDLE_VALUE;
    if( quietStdout )
    {
        SECURITY_ATTRIBUTES sa = { sizeof( sa ), nullptr, TRUE };
        nul = CreateFileA( "NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr );
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
        si.hStdOutput = nul;
        si.hStdError = GetStdHandle( STD_ERROR_HANDLE );
    }
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessA( exe.c_str(), cmd.data(), nullptr, nullptr, quietStdout ? TRUE : FALSE, 0, nullptr,
                                    nullptr, &si, &pi );
    if( nul != INVALID_HANDLE_VALUE ) CloseHandle( nul );
    if( !ok )
    {
        error = "could not start " + exe + " (error " + std::to_string( GetLastError() ) + ")";
        return false;
    }
    CloseHandle( pi.hThread );
    child.handle = pi.hProcess;
    return true;
}

int WaitProcess( ChildProcess& child, int timeoutMs )
{
    if( !child.handle ) return -1;
    if( WaitForSingleObject( child.handle, timeoutMs < 0 ? INFINITE : DWORD( timeoutMs ) ) != WAIT_OBJECT_0 )
    {
        return -1;
    }
    DWORD code = 0;
    GetExitCodeProcess( child.handle, &code );
    CloseHandle( child.handle );
    child.handle = nullptr;
    return int( code );
}

void KillProcess( ChildProcess& child )
{
    if( !child.handle ) return;
    TerminateProcess( child.handle, 1 );
    WaitForSingleObject( child.handle, 5000 );
    CloseHandle( child.handle );
    child.handle = nullptr;
}

std::string SelfPath()
{
    char path[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameA( nullptr, path, DWORD( sizeof( path ) ) );
    return std::string( path, n );
}

void SetEnvVar( const char* name, const std::string& value )
{
    _putenv_s( name, value.c_str() );
}

int FreeLocalPort()
{
    SOCKET s = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if( s == INVALID_SOCKET ) return 0;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    addr.sin_port = 0;
    int port = 0;
    int len = sizeof( addr );
    if( bind( s, (sockaddr*)&addr, sizeof( addr ) ) == 0 && getsockname( s, (sockaddr*)&addr, &len ) == 0 )
    {
        port = ntohs( addr.sin_port );
    }
    closesocket( s );
    return port;
}

static const char* kExeSuffix = ".exe";
static const char kPathSep = ';';

#else

bool ChildProcess::Valid() const
{
    return pid > 0;
}

bool SpawnProcess( const std::string& exe, const std::vector<std::string>& args, bool quietStdout,
                   ChildProcess& child, std::string& error )
{
    std::vector<char*> argv;
    argv.push_back( const_cast<char*>( exe.c_str() ) );
    for( auto& a : args ) argv.push_back( const_cast<char*>( a.c_str() ) );
    argv.push_back( nullptr );
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init( &actions );
    if( quietStdout ) posix_spawn_file_actions_addopen( &actions, 1, "/dev/null", O_WRONLY, 0 );
    pid_t pid = -1;
    const int rc = posix_spawn( &pid, exe.c_str(), &actions, nullptr, argv.data(), environ );
    posix_spawn_file_actions_destroy( &actions );
    if( rc != 0 )
    {
        error = "could not start " + exe;
        return false;
    }
    child.pid = pid;
    return true;
}

int WaitProcess( ChildProcess& child, int timeoutMs )
{
    if( child.pid <= 0 ) return -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeoutMs );
    for( ;; )
    {
        int status = 0;
        const pid_t r = waitpid( child.pid, &status, timeoutMs < 0 ? 0 : WNOHANG );
        if( r == child.pid )
        {
            child.pid = -1;
            return WIFEXITED( status ) ? WEXITSTATUS( status ) : 1;
        }
        if( timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline ) return -1;
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }
}

void KillProcess( ChildProcess& child )
{
    if( child.pid <= 0 ) return;
    kill( child.pid, SIGKILL );
    int status = 0;
    waitpid( child.pid, &status, 0 );
    child.pid = -1;
}

std::string SelfPath()
{
#  ifdef __APPLE__
    char path[4096];
    uint32_t size = sizeof( path );
    if( _NSGetExecutablePath( path, &size ) == 0 ) return fs::weakly_canonical( path ).string();
    return {};
#  else
    std::error_code ec;
    return fs::read_symlink( "/proc/self/exe", ec ).string();
#  endif
}

void SetEnvVar( const char* name, const std::string& value )
{
    setenv( name, value.c_str(), 1 );
}

int FreeLocalPort()
{
    const int s = socket( AF_INET, SOCK_STREAM, 0 );
    if( s < 0 ) return 0;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    socklen_t len = sizeof( addr );
    int port = 0;
    if( bind( s, (sockaddr*)&addr, sizeof( addr ) ) == 0 && getsockname( s, (sockaddr*)&addr, &len ) == 0 )
    {
        port = ntohs( addr.sin_port );
    }
    close( s );
    return port;
}

static const char* kExeSuffix = "";
static const char kPathSep = ':';

#endif

std::string FindTool( const std::string& explicitPath, const std::string& name )
{
    std::error_code ec;
    if( !explicitPath.empty() ) return fs::is_regular_file( explicitPath, ec ) ? explicitPath : std::string();
    const std::string fileName = name + kExeSuffix;
    const fs::path besideSelf = fs::path( SelfPath() ).parent_path() / fileName;
    if( fs::is_regular_file( besideSelf, ec ) ) return besideSelf.string();
    if( const char* path = std::getenv( "PATH" ) )
    {
        std::string all = path;
        size_t start = 0;
        while( start <= all.size() )
        {
            const size_t end = all.find( kPathSep, start );
            const std::string dir = all.substr( start, end == std::string::npos ? std::string::npos : end - start );
            if( !dir.empty() )
            {
                const fs::path candidate = fs::path( dir ) / fileName;
                if( fs::is_regular_file( candidate, ec ) ) return candidate.string();
            }
            if( end == std::string::npos ) break;
            start = end + 1;
        }
    }
    return {};
}

std::string FindCaptureTool( const std::string& explicitPath )
{
    return FindTool( explicitPath, "tracy-capture" );
}

}  // namespace tlb
