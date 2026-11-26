// tracy-roblox-bridge: receives Tracy.luau batches over localhost HTTP and replays
// them into the Tracy profiler, which connects to this process like any other
// Tracy-instrumented program.
//
//   tracy-roblox-bridge [--record session.tracy]      live (optionally recording)
//   tracy-roblox-bridge --convert in.tlrec out.tracy  turn a recording into a .tracy file
//   tracy-roblox-bridge --convert in.gprx out.tracy   ...or a MicroProfiler capture (needs Lute)

// httplib first: on Windows it has to include winsock2.h before windows.h.
#include "httplib.h"

#include "Process.hpp"
#include "Protocol.hpp"
#include "Recording.hpp"
#include "Replay.hpp"
#include "TracyEmit.hpp"

#include "common/TracyVersion.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{

httplib::Server* g_server = nullptr;
std::atomic<int> g_interrupts{ 0 };

void OnInterrupt()
{
    if( g_interrupts.fetch_add( 1 ) >= 1 )
    {
        std::fprintf( stderr, "\nsecond Ctrl+C: exiting now\n" );
        std::_Exit( 130 );
    }
    if( g_server ) g_server->stop();
}

#ifdef _WIN32
BOOL WINAPI OnConsoleCtrl( DWORD )
{
    OnInterrupt();
    return TRUE;
}
#else
void OnSignal( int )
{
    OnInterrupt();
}
#endif

void InstallInterruptHandler()
{
#ifdef _WIN32
    SetConsoleCtrlHandler( OnConsoleCtrl, TRUE );
#else
    std::signal( SIGINT, OnSignal );
    std::signal( SIGTERM, OnSignal );
#endif
}

struct Options
{
    std::string host = "127.0.0.1";
    int port = 8080;
    bool verbose = false;
    std::string record;  // live: also record to this .tracy file
    bool keepRaw = false;
    std::string captureTool;
    std::string convertIn, convertOut;  // --convert mode
    std::string captures = "captures";  // live: where POST /capture saves MicroProfiler captures
    bool convertCaptures = true;        // ...and turns them into .tracy files when it can
    std::string lute;                   // Lute, to run the MicroProfiler importer
    std::string importer;               // gprx2tlrec.luau
};

void PrintUsage()
{
    std::printf(
        "tracy-roblox-bridge " BRIDGE_VERSION "\n"
        "Streams Tracy.luau profiling data from Roblox Studio into the Tracy profiler.\n\n"
        "Usage:\n"
        "  tracy-roblox-bridge [--port N] [--host ADDR] [--record FILE.tracy] [--verbose]\n"
        "  tracy-roblox-bridge --convert IN.tlrec|IN.gprx OUT.tracy\n\n"
        "  --port N             HTTP port Tracy.luau posts to (default 8080)\n"
        "  --host ADDR          address to listen on (default 127.0.0.1; keep it local)\n"
        "  --record FILE.tracy  also save the session; written when the bridge stops (Ctrl+C).\n"
        "                       The live viewer keeps working while recording.\n"
        "  --keep-raw           keep the raw FILE.tlrec next to the .tracy file\n"
        "  --convert IN OUT     turn a raw .tlrec recording, or a MicroProfiler capture (.gprx,\n"
        "                       HTML dump, Tracy.Capture file), into a .tracy file\n"
        "  --captures DIR       where captures POSTed to /capture are saved (default: captures)\n"
        "  --no-convert-captures  save captures without converting them to .tracy\n"
        "  --lute PATH          Lute, for MicroProfiler captures (default: next to this program,\n"
        "                       then PATH)\n"
        "  --importer PATH      gprx2tlrec.luau (default: next to this program, then\n"
        "                       tools/microprofiler). LibMP.luau must sit beside it.\n"
        "  --capture-tool PATH  tracy-capture to write .tracy files with (default: next to\n"
        "                       this program, then PATH). Must be Tracy %d.%d.x.\n"
        "  --verbose            log every batch\n"
        "  --version            print versions and exit\n",
        tracy::Version::Major, tracy::Version::Minor );
}

bool ParseArgs( int argc, char** argv, Options& opt, int& exitCode )
{
    auto fail = [&]( const char* message ) {
        std::fprintf( stderr, "%s\n", message );
        exitCode = 2;
        return false;
    };
    for( int i = 1; i < argc; i++ )
    {
        const char* a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if( !std::strcmp( a, "--port" ) )
        {
            const char* v = value();
            const int port = v ? std::atoi( v ) : 0;
            if( port <= 0 || port > 65535 ) return fail( "--port needs a number from 1 to 65535" );
            opt.port = port;
        }
        else if( !std::strcmp( a, "--host" ) )
        {
            const char* v = value();
            if( !v ) return fail( "--host needs an address" );
            opt.host = v;
        }
        else if( !std::strcmp( a, "--record" ) )
        {
            const char* v = value();
            if( !v ) return fail( "--record needs a file name, like session.tracy" );
            opt.record = v;
        }
        else if( !std::strcmp( a, "--keep-raw" ) )
        {
            opt.keepRaw = true;
        }
        else if( !std::strcmp( a, "--capture-tool" ) )
        {
            const char* v = value();
            if( !v ) return fail( "--capture-tool needs the path to tracy-capture" );
            opt.captureTool = v;
        }
        else if( !std::strcmp( a, "--convert" ) )
        {
            const char* in = value();
            const char* out = value();
            if( !in || !out ) return fail( "--convert needs an input .tlrec and an output .tracy" );
            opt.convertIn = in;
            opt.convertOut = out;
        }
        else if( !std::strcmp( a, "--captures" ) )
        {
            const char* v = value();
            if( !v ) return fail( "--captures needs a folder" );
            opt.captures = v;
        }
        else if( !std::strcmp( a, "--no-convert-captures" ) )
        {
            opt.convertCaptures = false;
        }
        else if( !std::strcmp( a, "--lute" ) )
        {
            const char* v = value();
            if( !v ) return fail( "--lute needs the path to lute" );
            opt.lute = v;
        }
        else if( !std::strcmp( a, "--importer" ) )
        {
            const char* v = value();
            if( !v ) return fail( "--importer needs the path to gprx2tlrec.luau" );
            opt.importer = v;
        }
        else if( !std::strcmp( a, "--verbose" ) || !std::strcmp( a, "-v" ) )
        {
            opt.verbose = true;
        }
        else if( !std::strcmp( a, "--version" ) )
        {
            std::printf( "tracy-roblox-bridge %s (protocol %d, Tracy %d.%d.%d)\n", BRIDGE_VERSION,
                         int( tlb::kProtocolVersion ), tracy::Version::Major, tracy::Version::Minor,
                         tracy::Version::Patch );
            exitCode = 0;
            return false;
        }
        else if( !std::strcmp( a, "--help" ) || !std::strcmp( a, "-h" ) )
        {
            PrintUsage();
            exitCode = 0;
            return false;
        }
        else
        {
            std::fprintf( stderr, "unknown option %s\n\n", a );
            PrintUsage();
            exitCode = 2;
            return false;
        }
    }
    if( !opt.convertIn.empty() && !opt.record.empty() ) return fail( "--convert and --record don't go together" );
    return true;
}

std::string CaptureToolOrComplain( const std::string& explicitPath )
{
    const std::string tool = tlb::FindCaptureTool( explicitPath );
    if( tool.empty() )
    {
        std::fprintf( stderr,
                      "Writing .tracy files needs tracy-capture %d.%d.x (it ships in the same Tracy release zip as "
                      "the viewer). Put it next to tracy-roblox-bridge, add it to PATH, or pass --capture-tool.\n",
                      tracy::Version::Major, tracy::Version::Minor );
    }
    return tool;
}

std::string MegabytesOf( const std::string& path )
{
    std::error_code ec;
    const auto size = fs::file_size( path, ec );
    if( ec ) return "?";
    char text[32];
    std::snprintf( text, sizeof( text ), "%.1f MB", double( size ) / ( 1024.0 * 1024.0 ) );
    return text;
}

// ---------------------------------------------------------------- MicroProfiler import

bool IsRecordingFile( const std::string& path )
{
    std::ifstream f( path, std::ios::binary );
    char magic[8] = {};
    f.read( magic, sizeof( magic ) );
    return f.gcount() == 8 && std::memcmp( magic, "TLUAREC1", 8 ) == 0;
}

// gprx2tlrec.luau: --importer, else next to this program, else the repository's
// tools/microprofiler (for a bridge built in place), else under the current folder.
std::string FindImporter( const std::string& explicitPath )
{
    std::error_code ec;
    if( !explicitPath.empty() ) return fs::is_regular_file( explicitPath, ec ) ? explicitPath : std::string();
    const fs::path self = fs::path( tlb::SelfPath() ).parent_path();
    const fs::path candidates[] = {
        self / "gprx2tlrec.luau",
        self / ".." / "tools" / "microprofiler" / "gprx2tlrec.luau",
        fs::path( "tools" ) / "microprofiler" / "gprx2tlrec.luau",
    };
    for( const fs::path& c : candidates )
    {
        if( fs::is_regular_file( c, ec ) ) return fs::weakly_canonical( c, ec ).string();
    }
    return {};
}

struct ImportTools
{
    std::string lute, importer;
    std::string problem;  // empty when both were found, with LibMP beside the importer
};

ImportTools FindImportTools( const Options& opt )
{
    ImportTools t;
    t.lute = tlb::FindTool( opt.lute, "lute" );
    t.importer = FindImporter( opt.importer );
    std::error_code ec;
    if( t.lute.empty() )
        t.problem = "Lute isn't installed (https://github.com/luau-lang/lute/releases): put lute next to "
                    "tracy-roblox-bridge, add it to PATH, or pass --lute";
    else if( t.importer.empty() )
        t.problem = "gprx2tlrec.luau wasn't found: put it next to tracy-roblox-bridge or pass --importer";
    else if( !fs::is_regular_file( fs::path( t.importer ).parent_path() / "LibMP.luau", ec ) )
        t.problem = "LibMP.luau isn't next to " + t.importer +
                    " (download it from https://github.com/Roblox/libmp, MIT)";
    return t;
}

// Runs the MicroProfiler importer: capture -> .tlrec recording.
bool ImportCapture( const ImportTools& tools, const std::string& in, const std::string& tlrec, std::string& error )
{
    tlb::ChildProcess lute;
    // --quiet: the importer prints only problems, which show on our console.
    if( !tlb::SpawnProcess( tools.lute, { "run", tools.importer, in, tlrec, "--quiet" }, false, lute, error ) )
        return false;
    const int code = tlb::WaitProcess( lute, -1 );
    std::error_code ec;
    if( code != 0 || !fs::is_regular_file( tlrec, ec ) )
    {
        error = "the MicroProfiler importer failed (exit " + std::to_string( code ) + ") on " + in;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- --convert

// How far before a request's arrival its earliest event can land on the replay
// clock. A batch is stamped when sealed, but its events can be a flush interval older,
// or much older after an outage. Tracy drops zones from before the trace starts, so
// the replay clock starts this far ahead of the capture's connection.
int64_t ReplayLeadNs( const std::string& path )
{
    tlb::RecordingReader reader;
    std::string error;
    if( !reader.Open( path, error ) ) return 0;
    int64_t lead = 0;
    int64_t arrival = 0;
    std::string body;
    while( reader.Next( arrival, body ) )
    {
        tlb::Reader r( body.data(), body.size() );
        bool haveRef = false;
        double refFlush = 0;
        while( !r.Empty() )
        {
            const size_t start = r.Offset();
            tlb::BatchHeader h;
            const char* bad = nullptr;
            if( !tlb::ParseHeader( r, h, bad ) ) break;
            r.Skip( h.length );
            if( !haveRef )
            {
                // The request's first batch anchors it (the server's, when relaying).
                haveRef = true;
                refFlush = h.flushClock + h.clockOffset;
            }
            double earliest = 0;
            if( tlb::EarliestEventTime( body.data() + start + tlb::kHeaderSize, h.length, earliest ) )
            {
                const double before = refFlush - ( earliest + h.clockOffset );
                const int64_t need = int64_t( before * 1e9 ) - arrival;
                if( need > lead && before < 1e6 ) lead = need;
            }
        }
    }
    return lead;
}

// Replays a recording into a private Tracy client that tracy-capture is connected
// to. Requests keep their recorded spacing on a virtual clock, so the result is the
// same timeline the live viewer would have shown, produced as fast as it can be read.
int RunConvert( const Options& opt )
{
    const std::string tool = CaptureToolOrComplain( opt.captureTool );
    if( tool.empty() ) return 1;

    std::string input = opt.convertIn;
    std::string imported;  // the .tlrec made from a MicroProfiler capture; removed unless --keep-raw
    std::error_code ec;
    if( !fs::is_regular_file( input, ec ) )
    {
        std::fprintf( stderr, "%s doesn't exist\n", input.c_str() );
        return 1;
    }
    if( !IsRecordingFile( input ) )
    {
        const ImportTools tools = FindImportTools( opt );
        if( !tools.problem.empty() )
        {
            std::fprintf( stderr,
                          "%s isn't a Tracy-Luau recording, so it was read as a MicroProfiler capture, but %s.\n",
                          input.c_str(), tools.problem.c_str() );
            return 1;
        }
        imported = fs::path( opt.convertOut ).replace_extension( ".import.tlrec" ).string();
        std::string importError;
        if( !ImportCapture( tools, input, imported, importError ) )
        {
            std::fprintf( stderr, "%s\n", importError.c_str() );
            fs::remove( imported, ec );
            return 1;
        }
        input = imported;
    }
    struct RemoveImported
    {
        std::string path;
        bool keep;
        ~RemoveImported()
        {
            std::error_code e;
            if( !path.empty() && !keep ) fs::remove( path, e );
        }
    } removeImported{ imported, opt.keepRaw };

    tlb::RecordingReader reader;
    std::string error;
    if( !reader.Open( input, error ) )
    {
        std::fprintf( stderr, "%s\n", error.c_str() );
        return 1;
    }
    const int64_t lead = ReplayLeadNs( input );

    // A port of its own, so a live bridge and its viewer can keep running.
    const int port = tlb::FreeLocalPort();
    if( port <= 0 )
    {
        std::fprintf( stderr, "could not find a free local port for the capture\n" );
        return 1;
    }
    tlb::SetEnvVar( "TRACY_PORT", std::to_string( port ) );
    tracy::StartupProfiler();
    const std::string appInfo = "Tracy-Luau recording " + fs::path( opt.convertIn ).filename().string();
    tracy::Profiler::MessageAppInfo( appInfo.data(), appInfo.size() );

    tlb::ChildProcess capture;
    if( !tlb::SpawnProcess( tool, { "-o", opt.convertOut, "-a", "127.0.0.1", "-p", std::to_string( port ), "-f" },
                            true, capture, error ) )
    {
        std::fprintf( stderr, "%s\n", error.c_str() );
        tracy::ShutdownProfiler();
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 20 );
    while( !tlb::emit::ViewerConnected() && std::chrono::steady_clock::now() < deadline )
    {
        if( tlb::WaitProcess( capture, 0 ) != -1 ) break;  // capture exited (wrong Tracy version?)
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }
    if( !tlb::emit::ViewerConnected() )
    {
        std::fprintf( stderr, "tracy-capture didn't connect. Is it Tracy %d.%d.x? (%s)\n", tracy::Version::Major,
                      tracy::Version::Minor, tool.c_str() );
        tlb::KillProcess( capture );
        tracy::ShutdownProfiler();
        return 1;
    }

    tlb::Interner interner;
    tlb::SrcLocCache srclocs;
    tlb::Router router( interner, srclocs, opt.verbose, !opt.verbose );
    // +50 ms covers latency jitter between requests, which the lead estimate ignores.
    const int64_t base = tlb::emit::Now() + lead + 50'000'000;
    int64_t arrival = 0;
    std::string body;
    uint64_t requests = 0, rejected = 0, bytes = 0;
    int64_t lastArrival = 0;
    while( reader.Next( arrival, body ) )
    {
        const tlb::RequestOutcome outcome = tlb::IngestRequest( router, body, base + arrival );
        requests++;
        bytes += body.size();
        lastArrival = arrival;
        if( outcome.status == 400 ) rejected++;
    }
    const std::string problems = router.ProblemReport();
    router.Shutdown();
    tracy::ShutdownProfiler();  // sends everything, then waits until tracy-capture has it all
    if( !problems.empty() ) std::printf( "the recording had some damage:\n%s", problems.c_str() );

    const int code = tlb::WaitProcess( capture, 120000 );
    if( code == -1 ) tlb::KillProcess( capture );
    if( code != 0 || !fs::exists( opt.convertOut, ec ) )
    {
        std::fprintf( stderr, "tracy-capture failed (exit %d); %s was not written\n", code, opt.convertOut.c_str() );
        return 1;
    }
    std::printf( "wrote %s (%s): %llu requests, %.1f MB of events, %.1f s of profiling%s%s\n", opt.convertOut.c_str(),
                 MegabytesOf( opt.convertOut ).c_str(), (unsigned long long)requests, double( bytes ) / ( 1024.0 * 1024.0 ),
                 double( lastArrival ) / 1e9, reader.Truncated() ? "; the recording's last request was cut short" : "",
                 rejected ? "; some requests were malformed and skipped" : "" );
    return 0;
}

// ---------------------------------------------------------------- live: captures

// Standard base64 (padding optional). Empty on any character outside the alphabet.
std::string Base64Decode( const std::string& in )
{
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for( const char c : in )
    {
        int v;
        if( c >= 'A' && c <= 'Z' ) v = c - 'A';
        else if( c >= 'a' && c <= 'z' ) v = c - 'a' + 26;
        else if( c >= '0' && c <= '9' ) v = c - '0' + 52;
        else if( c == '+' ) v = 62;
        else if( c == '/' ) v = 63;
        else if( c == '=' ) break;
        else return {};
        acc = ( acc << 6 ) | uint32_t( v );
        bits += 6;
        if( bits >= 8 )
        {
            bits -= 8;
            out += char( ( acc >> bits ) & 0xFF );
        }
    }
    return out;
}

// X-Tracy-Capture: the capture info JSON. Tracy.Capture sends it base64-encoded,
// because HttpService refuses "{" in header values; plain JSON is accepted too.
std::string CaptureInfoFrom( const std::string& header )
{
    if( header.empty() || header[0] == '{' ) return header;
    return Base64Decode( header );
}

// The capture's id from its X-Tracy-Capture JSON ("Id":"..."), if it's a plain
// token; otherwise a timestamp. Never anything that could leave the folder.
std::string CaptureFileStem( const std::string& info )
{
    std::string id;
    size_t at = info.find( "\"Id\"" );
    if( at != std::string::npos )
    {
        // "Id" : "value", with any whitespace around the colon
        at += 4;
        while( at < info.size() && std::isspace( (unsigned char)info[at] ) ) at++;
        if( at < info.size() && info[at] == ':' ) at++;
        while( at < info.size() && std::isspace( (unsigned char)info[at] ) ) at++;
        if( at >= info.size() || info[at] != '"' ) at = info.size();
        for( size_t i = at + 1; i < info.size() && info[i] != '"' && id.size() < 64; i++ )
        {
            const char c = info[i];
            if( !( std::isalnum( (unsigned char)c ) || c == '-' || c == '_' ) )
            {
                id.clear();
                break;
            }
            id += c;
        }
    }
    if( !id.empty() ) return id;
    char stamp[32];
    const std::time_t now = std::time( nullptr );
    std::strftime( stamp, sizeof( stamp ), "capture-%Y%m%dT%H%M%SZ", std::gmtime( &now ) );
    return stamp;
}

// ---------------------------------------------------------------- live

int RunLive( const Options& opt )
{
    std::string recordTool;
    std::string rawPath;
    tlb::Recorder recorder;
    if( !opt.record.empty() )
    {
        // Check for tracy-capture up front, not after an hour of recording.
        recordTool = CaptureToolOrComplain( opt.captureTool );
        if( recordTool.empty() ) return 1;
        rawPath = tlb::RawPathFor( opt.record );
        std::string error;
        if( !recorder.Open( rawPath, error ) )
        {
            std::fprintf( stderr, "%s\n", error.c_str() );
            return 1;
        }
    }

    tracy::StartupProfiler();
    const std::string appInfo = "Tracy-Luau bridge " BRIDGE_VERSION " (protocol " +
                                std::to_string( tlb::kProtocolVersion ) + ")";
    tracy::Profiler::MessageAppInfo( appInfo.data(), appInfo.size() );

    tlb::Interner interner;
    tlb::SrcLocCache srclocs;
    tlb::Router router( interner, srclocs, opt.verbose );

    httplib::Server server;
    server.set_payload_max_length( 64 * 1024 * 1024 );

    server.Post( "/ingest", [&]( const httplib::Request& req, httplib::Response& res ) {
        // A web page can send a plain cross-site POST to localhost, but not one with
        // this content type (that needs a CORS preflight, which we never approve).
        if( req.get_header_value( "Content-Type" ).rfind( "application/octet-stream", 0 ) != 0 )
        {
            res.status = 415;
            res.set_content( "Content-Type must be application/octet-stream", "text/plain" );
            return;
        }
        const int64_t now = tlb::emit::Now();
        if( recorder.IsOpen() ) recorder.Write( now, req.body );
        const tlb::RequestOutcome outcome = tlb::IngestRequest( router, req.body, now );
        res.status = outcome.status;
        if( !outcome.text.empty() ) res.set_content( outcome.text, "text/plain" );
    } );

    // MicroProfiler captures from Tracy.Capture's BridgeSink. Saved as-is, then
    // converted to .tracy in the background when Lute, the importer and tracy-capture
    // were all found.
    const ImportTools importTools = FindImportTools( opt );
    const std::string captureTool = tlb::FindCaptureTool( opt.captureTool );
    const bool convertCaptures = opt.convertCaptures && importTools.problem.empty() && !captureTool.empty();
    const fs::path capturesDir = fs::absolute( opt.captures );
    server.Post( "/capture", [&]( const httplib::Request& req, httplib::Response& res ) {
        if( req.get_header_value( "Content-Type" ).rfind( "application/octet-stream", 0 ) != 0 )
        {
            res.status = 415;
            res.set_content( "Content-Type must be application/octet-stream", "text/plain" );
            return;
        }
        if( req.body.empty() )
        {
            res.status = 400;
            res.set_content( "empty capture", "text/plain" );
            return;
        }
        std::error_code ec;
        fs::create_directories( capturesDir, ec );
        const std::string info = CaptureInfoFrom( req.get_header_value( "X-Tracy-Capture" ) );
        const std::string stem = CaptureFileStem( info );
        fs::path path = capturesDir / ( stem + ".gprx" );
        for( int n = 2; fs::exists( path, ec ); n++ )
            path = capturesDir / ( stem + "-" + std::to_string( n ) + ".gprx" );
        {
            std::ofstream out( path, std::ios::binary );
            out.write( req.body.data(), std::streamsize( req.body.size() ) );
            if( !out )
            {
                res.status = 500;
                res.set_content( "could not write " + path.string(), "text/plain" );
                return;
            }
        }
        if( !info.empty() )
        {
            std::ofstream meta( fs::path( path ).replace_extension( ".json" ), std::ios::binary );
            meta << info;
        }
        std::printf( "capture saved: %s (%s)\n", path.string().c_str(), MegabytesOf( path.string() ).c_str() );
        res.set_content( "saved " + path.filename().string(), "text/plain" );
        if( !convertCaptures ) return;
        // A fresh process converts it: this one's Tracy client is the live viewer's.
        const std::string out = fs::path( path ).replace_extension( ".tracy" ).string();
        std::vector<std::string> args = { "--convert", path.string(), out, "--capture-tool", captureTool,
                                          "--lute", importTools.lute, "--importer", importTools.importer };
        std::thread( [args, out]() {
            tlb::ChildProcess convert;
            std::string error;
            if( !tlb::SpawnProcess( tlb::SelfPath(), args, true, convert, error ) )
            {
                std::fprintf( stderr, "capture conversion failed: %s\n", error.c_str() );
                return;
            }
            const int code = tlb::WaitProcess( convert, -1 );
            if( code == 0 )
                std::printf( "capture converted: %s\n", out.c_str() );
            else
                std::fprintf( stderr, "capture conversion failed (exit %d) for %s\n", code, out.c_str() );
        } ).detach();
    } );

    server.Get( "/status", [&]( const httplib::Request&, httplib::Response& res ) {
        res.set_content( router.StatusJson( tlb::emit::ViewerConnected() ), "application/json" );
    } );

    if( !server.bind_to_port( opt.host, opt.port ) )
    {
        std::fprintf( stderr,
                      "Could not listen on %s:%d. Another program may be using the port; try --port and set the same "
                      "port in Tracy.Configure({ Url = ... }).\n",
                      opt.host.c_str(), opt.port );
        recorder.Close();
        std::error_code ec;
        fs::remove( rawPath, ec );
        tracy::ShutdownProfiler();
        return 1;
    }

    std::printf( "tracy-roblox-bridge %s (protocol %d, Tracy %d.%d.%d)\n", BRIDGE_VERSION, int( tlb::kProtocolVersion ),
                 tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch );
    std::printf( "  Roblox posts to  http://%s:%d/ingest\n", opt.host.c_str(), opt.port );
    std::printf( "  Tracy viewer     open Tracy %d.%d.x and connect to 127.0.0.1\n", tracy::Version::Major,
                 tracy::Version::Minor );
    if( recorder.IsOpen() ) std::printf( "  Recording        %s (saved when you stop the bridge)\n", opt.record.c_str() );
    const char* captureNote = convertCaptures          ? " and converted to .tracy"
                              : !opt.convertCaptures   ? ""
                              : captureTool.empty()    ? " (not converted: tracy-capture not found)"
                                                       : " (not converted: Lute, gprx2tlrec.luau or LibMP missing)";
    std::printf( "  Captures         saved to %s%s\n", capturesDir.string().c_str(), captureNote );
    std::printf( "  Ctrl+C to stop\n" );

    g_server = &server;
    InstallInterruptHandler();
    server.listen_after_bind();
    g_server = nullptr;

    router.Shutdown();
    recorder.Close();
    std::printf( "bridge stopped\n" );
    // If a viewer is connected, this waits until it has received everything.
    tracy::ShutdownProfiler();

    if( !rawPath.empty() )
    {
        if( recorder.Records() == 0 )
        {
            std::printf( "nothing was recorded; no %s written\n", opt.record.c_str() );
            std::error_code ec;
            fs::remove( rawPath, ec );
            return 0;
        }
        std::printf( "writing %s from %llu requests...\n", opt.record.c_str(), (unsigned long long)recorder.Records() );
        tlb::ChildProcess convert;
        std::string error;
        if( !tlb::SpawnProcess( tlb::SelfPath(),
                                { "--convert", rawPath, opt.record, "--capture-tool", recordTool }, false, convert,
                                error ) )
        {
            std::fprintf( stderr, "%s\nThe raw recording is kept: %s\n", error.c_str(), rawPath.c_str() );
            return 1;
        }
        const int code = tlb::WaitProcess( convert, -1 );
        if( code != 0 )
        {
            std::fprintf( stderr, "conversion failed. The raw recording is kept; retry with:\n  %s --convert \"%s\" \"%s\"\n",
                          tlb::SelfPath().c_str(), rawPath.c_str(), opt.record.c_str() );
            return 1;
        }
        if( !opt.keepRaw )
        {
            std::error_code ec;
            fs::remove( rawPath, ec );
        }
    }
    return 0;
}

}  // namespace

int main( int argc, char** argv )
{
    Options opt;
    int exitCode = 0;
    if( !ParseArgs( argc, argv, opt, exitCode ) ) return exitCode;

    // Unbuffered so log lines show up immediately when piped or run from a tool.
    std::setvbuf( stdout, nullptr, _IONBF, 0 );

    if( !opt.convertIn.empty() )
    {
        InstallInterruptHandler();
        return RunConvert( opt );
    }
    return RunLive( opt );
}
