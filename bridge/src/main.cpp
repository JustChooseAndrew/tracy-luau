// tracy-roblox-bridge: receives Tracy.luau batches over localhost HTTP and replays
// them into the Tracy profiler, which connects to this process like any other
// Tracy-instrumented program.
//
//   tracy-roblox-bridge [--record session.tracy]      live (optionally recording)
//   tracy-roblox-bridge --convert in.tlrec out.tracy  turn a recording into a .tracy file

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
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

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
};

void PrintUsage()
{
    std::printf(
        "tracy-roblox-bridge " BRIDGE_VERSION "\n"
        "Streams Tracy.luau profiling data from Roblox Studio into the Tracy profiler.\n\n"
        "Usage:\n"
        "  tracy-roblox-bridge [--port N] [--host ADDR] [--record FILE.tracy] [--verbose]\n"
        "  tracy-roblox-bridge --convert IN.tlrec OUT.tracy\n\n"
        "  --port N             HTTP port Tracy.luau posts to (default 8080)\n"
        "  --host ADDR          address to listen on (default 127.0.0.1; keep it local)\n"
        "  --record FILE.tracy  also save the session; written when the bridge stops (Ctrl+C).\n"
        "                       The live viewer keeps working while recording.\n"
        "  --keep-raw           keep the raw FILE.tlrec next to the .tracy file\n"
        "  --convert IN OUT     turn a raw .tlrec recording into a .tracy file\n"
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

// ---------------------------------------------------------------- --convert

// How far before a request's arrival its earliest event can land on the replay
// clock. A batch is stamped when sealed, but its events happened up to a flush (or,
// after an outage, much longer) earlier. Starting the replay clock this far ahead of
// the capture's connection keeps every event inside the trace: Tracy drops zones
// that fall before the trace starts.
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

    tlb::RecordingReader reader;
    std::string error;
    if( !reader.Open( opt.convertIn, error ) )
    {
        std::fprintf( stderr, "%s\n", error.c_str() );
        return 1;
    }
    const int64_t lead = ReplayLeadNs( opt.convertIn );

    // A port of our own, so a live bridge (and its viewer) can keep running meanwhile.
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
    router.Shutdown();         // end every track, closing open zones
    tracy::ShutdownProfiler();  // sends everything, then waits until tracy-capture has it all
    if( !problems.empty() ) std::printf( "the recording had some damage:\n%s", problems.c_str() );

    const int code = tlb::WaitProcess( capture, 120000 );
    if( code == -1 ) tlb::KillProcess( capture );
    std::error_code ec;
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

    // One request holds one or more batches back to back: the server's own, then any
    // it relayed from clients (docs/PROTOCOL.md).
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
        // A fresh process converts it: this one's Tracy client was the live viewer's.
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
