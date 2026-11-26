#include "Replay.hpp"

#include "TracyEmit.hpp"

#include "common/TracySystem.hpp"
#include "common/TracyVersion.hpp"

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <limits>

namespace tlb
{

namespace
{

constexpr uint64_t kNotSent = std::numeric_limits<uint64_t>::max();

// A lane whose session has been silent this long is free for the next playtest.
constexpr int64_t kIdleNs = 5'000'000'000;

int64_t SecondsToNs( double seconds )
{
    return int64_t( std::llround( seconds * 1e9 ) );
}

// os.clock() is seconds since the Roblox process started, so anything non-finite or
// beyond ~3 years is corrupt. One such value would stretch the whole track, so the
// batch is rejected.
bool ValidTime( double seconds )
{
    return std::isfinite( seconds ) && seconds > -1e8 && seconds < 1e8;
}

void AppendJsonString( std::string& out, std::string_view s )
{
    out += '"';
    for( char c : s )
    {
        if( c == '"' || c == '\\' )
        {
            out += '\\';
            out += c;
        }
        else if( uint8_t( c ) < 0x20 )
        {
            char esc[8];
            std::snprintf( esc, sizeof( esc ), "\\u%04x", unsigned( uint8_t( c ) ) );
            out += esc;
        }
        else
        {
            out += c;
        }
    }
    out += '"';
}

}  // namespace

// ---------------------------------------------------------------- Interner

const char* Interner::Intern( std::string_view text )
{
    std::lock_guard<std::mutex> lock( m_lock );
    return m_strings.emplace( text ).first->c_str();
}

const tracy::SourceLocationData* SrcLocCache::Get( const char* name, const char* file, uint32_t color )
{
    std::lock_guard<std::mutex> lock( m_lock );
    auto& slot = m_locs[{ name, file, color }];
    if( !slot ) slot = new tracy::SourceLocationData{ name, name, file, 0, color };
    return slot;
}

// ---------------------------------------------------------------- Lane

Lane::Lane( uint8_t source, std::string label, std::string shortLabel, const char* frameName, int32_t groupHint,
            Interner& interner, SrcLocCache& srclocs, bool verbose, bool quiet )
    : m_source( source ),
      m_label( std::move( label ) ),
      m_shortLabel( std::move( shortLabel ) ),
      m_labelInterned( interner.Intern( m_label ) ),
      m_frameName( frameName ),
      m_groupHint( groupHint ),
      m_interner( interner ),
      m_srclocs( srclocs ),
      m_verbose( verbose ),
      m_quiet( quiet ),
      m_thread( [this] { Run(); } )
{
}

Lane::~Lane()
{
    {
        std::lock_guard<std::mutex> lock( m_jobLock );
        m_stop = true;
    }
    m_jobReady.notify_one();
    m_thread.join();
}

IngestResult Lane::Submit( std::string body, const BatchHeader& header, ClockRef ref, int64_t nowNs )
{
    auto job = std::make_shared<Job>();
    job->body = std::move( body );
    job->header = header;
    job->ref = ref;
    job->nowNs = nowNs;
    auto done = job->done.get_future();
    {
        std::lock_guard<std::mutex> lock( m_jobLock );
        m_jobs.push_back( std::move( job ) );
    }
    m_jobReady.notify_one();
    return done.get();
}

LaneStats Lane::Stats()
{
    std::lock_guard<std::mutex> lock( m_statsLock );
    return m_stats;
}

void Lane::Run()
{
    tracy::SetThreadNameWithHint( m_labelInterned, m_groupHint );
    for( ;; )
    {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock( m_jobLock );
            m_jobReady.wait( lock, [this] { return m_stop || !m_jobs.empty(); } );
            if( m_jobs.empty() ) break;  // stopping, and nothing left to replay
            job = std::move( m_jobs.front() );
            m_jobs.pop_front();
        }
        job->done.set_value( Replay( job->body, job->header, job->ref, job->nowNs ) );
    }
    CloseOpenZones();
}

void Lane::Rebind( const BatchHeader& header )
{
    CloseOpenZones();
    if( !m_quiet ) std::printf( "[%s] session %08" PRIx32 " started\n", m_label.c_str(), header.session );
    m_bound = true;
    m_session = header.session;
    m_expectedSeq = header.sequence;  // a bridge restart can join a session mid-stream
    m_haveOffset = false;
    m_names.clear();
    m_zoneLocs.clear();
    m_prefixedNames.clear();
    m_colorLocs.clear();
}

void Lane::SetName( uint16_t id, const char* name )
{
    if( id >= m_names.size() )
    {
        m_names.resize( size_t( id ) + 1, nullptr );
        m_zoneLocs.resize( size_t( id ) + 1, nullptr );
        m_prefixedNames.resize( size_t( id ) + 1, nullptr );
    }
    if( m_names[id] == name ) return;  // re-registration of the same text
    m_names[id] = name;
    // rebuilt from the new name on next use
    m_zoneLocs[id] = nullptr;
    m_prefixedNames[id] = nullptr;
    if( !m_colorLocs.empty() ) m_colorLocs.clear();
}

const char* Lane::PrefixedNameFor( uint16_t id, bool& unknown )
{
    if( id < m_prefixedNames.size() && m_prefixedNames[id] ) return m_prefixedNames[id];
    const char* name = NameFor( id, unknown );
    const std::string full = m_shortLabel + ": " + name;
    const char* interned = m_interner.Intern( full );
    if( id < m_prefixedNames.size() && m_names[id] ) m_prefixedNames[id] = interned;
    return interned;
}

const tracy::SourceLocationData* Lane::ColoredSrcLocFor( uint16_t id, uint32_t color, bool& unknown )
{
    const uint64_t key = ( uint64_t( id ) << 32 ) | color;
    if( auto it = m_colorLocs.find( key ); it != m_colorLocs.end() ) return it->second;
    const char* name = NameFor( id, unknown );
    auto loc = m_srclocs.Get( name, m_labelInterned, color );
    if( id < m_names.size() && m_names[id] ) m_colorLocs.emplace( key, loc );
    return loc;
}

const char* Lane::NameFor( uint16_t id, bool& unknown )
{
    if( id < m_names.size() && m_names[id] ) return m_names[id];
    unknown = true;
    char text[32];
    std::snprintf( text, sizeof( text ), "<unregistered #%u>", unsigned( id ) );
    return m_interner.Intern( text );
}

const tracy::SourceLocationData* Lane::SrcLocFor( uint16_t id, bool& unknown )
{
    if( id < m_zoneLocs.size() && m_zoneLocs[id] ) return m_zoneLocs[id];
    const char* name = NameFor( id, unknown );
    auto loc = m_srclocs.Get( name, m_labelInterned, 0 );
    if( id < m_zoneLocs.size() && m_names[id] ) m_zoneLocs[id] = loc;
    return loc;
}

int64_t Lane::Map( double luauSeconds )
{
    int64_t t = m_offsetNs + SecondsToNs( luauSeconds );
    // os.clock() never goes backwards inside one VM, but a new session on a reused
    // lane must not land before the previous session's last event on this track.
    if( t < m_lastTime ) t = m_lastTime;
    m_lastTime = t;
    return t;
}

void Lane::ZoneBegin( const tracy::SourceLocationData* srcloc, int64_t time )
{
    uint64_t sentTo = kNotSent;
    if( emit::ViewerConnected() )
    {
        sentTo = emit::ConnectionId();
        emit::ZoneBegin( srcloc, time );
    }
    m_zoneStack.push_back( { time, sentTo } );
}

void Lane::ZoneEnd( int64_t time )
{
    if( m_zoneStack.empty() )
    {
        std::lock_guard<std::mutex> lock( m_statsLock );
        m_stats.droppedEnds++;
        return;
    }
    const OpenZone zone = m_zoneStack.back();
    m_zoneStack.pop_back();
    // os.clock() ticks every 100 ns, so a short zone can begin and end on the same
    // tick. Tracy leaves zero-length zones out of its statistics, so they get 1 ns,
    // well inside the timer's error.
    if( time <= zone.begin )
    {
        time = zone.begin + 1;
        if( time > m_lastTime ) m_lastTime = time;
    }
    if( zone.sentTo != kNotSent && emit::ViewerConnected() && emit::ConnectionId() == zone.sentTo )
    {
        emit::ZoneEnd( time );
    }
}

void Lane::CloseOpenZones()
{
    while( !m_zoneStack.empty() ) ZoneEnd( m_lastTime );
}

IngestResult Lane::Replay( const std::string& body, const BatchHeader& header, ClockRef ref, int64_t nowNs )
{
    if( !ValidTime( header.flushClock ) || !ValidTime( header.clockOffset ) )
    {
        std::lock_guard<std::mutex> lock( m_statsLock );
        m_stats.malformed++;
        return { 400, "bad flushClock or clockOffset in header" };
    }

    uint64_t gaps = 0;
    if( !m_bound || header.session != m_session )
    {
        Rebind( header );
    }
    else if( header.sequence != m_expectedSeq )
    {
        // Batches were lost (HTTP failure or a full Luau buffer). Their ZoneBegin /
        // ZoneEnd pairs are gone, so close what is open to keep the track balanced.
        gaps = uint32_t( header.sequence - m_expectedSeq );
        CloseOpenZones();
    }
    m_expectedSeq = header.sequence + 1;

    if( header.source != SourceServer && ref.valid )
    {
        // A relayed client batch. Its clockOffset puts its times on the server's clock,
        // and the server session's offset puts those on Tracy's, so client and server
        // tracks line up.
        m_offsetNs = ref.offsetNs + SecondsToNs( header.clockOffset );
        m_haveOffset = true;
    }
    else if( !m_haveOffset )
    {
        // Pin Luau's clock to ours: the batch was sealed "now", minus transit time,
        // which on localhost is well under a millisecond. Fixed for the session, so
        // durations are exactly what Luau measured.
        m_offsetNs = nowNs - SecondsToNs( header.flushClock + header.clockOffset );
        m_haveOffset = true;
    }

    const bool connected = emit::ViewerConnected();
    Reader r( body.data() + kHeaderSize, body.size() - kHeaderSize );
    const char* error = nullptr;
    bool unknown = false;
    uint64_t events = 0;

    while( !r.Empty() && !error )
    {
        uint8_t op = 0;
        r.Read( op );
        switch( op )
        {
        case OpRegisterString:
        {
            uint16_t id = 0, len = 0;
            std::string_view text;
            if( !r.Read( id ) || !r.Read( len ) || !r.ReadBytes( len, text ) )
            {
                error = "truncated REGISTER_STRING";
                break;
            }
            SetName( id, m_interner.Intern( text ) );
            break;
        }
        case OpZoneBegin:
        {
            uint16_t id = 0;
            double t = 0;
            if( !r.Read( id ) || !r.Read( t ) )
            {
                error = "truncated ZONE_BEGIN";
                break;
            }
            if( !ValidTime( t ) )
            {
                error = "bad timestamp in ZONE_BEGIN";
                break;
            }
            ZoneBegin( SrcLocFor( id, unknown ), Map( t ) );
            break;
        }
        case OpZoneBeginColor:
        {
            uint16_t id = 0;
            double t = 0;
            uint32_t color = 0;
            if( !r.Read( id ) || !r.Read( t ) || !r.Read( color ) )
            {
                error = "truncated ZONE_BEGIN_COLOR";
                break;
            }
            if( !ValidTime( t ) )
            {
                error = "bad timestamp in ZONE_BEGIN_COLOR";
                break;
            }
            color &= 0xFFFFFF;
            ZoneBegin( color ? ColoredSrcLocFor( id, color, unknown ) : SrcLocFor( id, unknown ), Map( t ) );
            break;
        }
        case OpMessage:
        {
            double t = 0;
            uint32_t color = 0;
            uint16_t len = 0;
            std::string_view text;
            if( !r.Read( t ) || !r.Read( color ) || !r.Read( len ) || !r.ReadBytes( len, text ) )
            {
                error = "truncated MESSAGE";
                break;
            }
            if( !ValidTime( t ) )
            {
                error = "bad timestamp in MESSAGE";
                break;
            }
            const int64_t time = Map( t );
            if( connected && !text.empty() ) emit::Message( text.data(), text.size(), color & 0xFFFFFF, time );
            break;
        }
        case OpTrackName:
        {
            uint16_t len = 0;
            std::string_view name;
            if( !r.Read( len ) || !r.ReadBytes( len, name ) )
            {
                error = "truncated TRACK_NAME";
                break;
            }
            break;  // already used to pick the track when the batch was routed
        }
        case OpZoneText:
        {
            uint16_t len = 0;
            std::string_view text;
            if( !r.Read( len ) || !r.ReadBytes( len, text ) )
            {
                error = "truncated ZONE_TEXT";
                break;
            }
            // Only onto a zone the current viewer has seen begin.
            if( !text.empty() && !m_zoneStack.empty() && m_zoneStack.back().sentTo != kNotSent &&
                emit::ViewerConnected() && emit::ConnectionId() == m_zoneStack.back().sentTo )
            {
                emit::ZoneText( text.data(), text.size() );
            }
            break;
        }
        case OpPlotConfig:
        {
            uint16_t id = 0;
            uint8_t format = 0, plotFlags = 0;
            uint32_t color = 0;
            if( !r.Read( id ) || !r.Read( format ) || !r.Read( plotFlags ) || !r.Read( color ) )
            {
                error = "truncated PLOT_CONFIG";
                break;
            }
            if( format > 2 )
            {
                error = "unknown plot format in PLOT_CONFIG";
                break;
            }
            emit::ConfigurePlot( PrefixedNameFor( id, unknown ), format, ( plotFlags & PlotFlagStep ) != 0,
                                 ( plotFlags & PlotFlagFill ) != 0, color & 0xFFFFFF );
            break;
        }
        case OpZoneEnd:
        {
            double t = 0;
            if( !r.Read( t ) )
            {
                error = "truncated ZONE_END";
                break;
            }
            if( !ValidTime( t ) )
            {
                error = "bad timestamp in ZONE_END";
                break;
            }
            ZoneEnd( Map( t ) );
            break;
        }
        case OpFrameMark:
        {
            double t = 0;
            if( !r.Read( t ) )
            {
                error = "truncated FRAME_MARK";
                break;
            }
            if( !ValidTime( t ) )
            {
                error = "bad timestamp in FRAME_MARK";
                break;
            }
            const int64_t time = Map( t );
            if( connected ) emit::FrameMark( m_frameName, time );
            break;
        }
        case OpFrameMarkNamed:
        {
            uint16_t id = 0;
            double t = 0;
            if( !r.Read( id ) || !r.Read( t ) )
            {
                error = "truncated FRAME_MARK_NAMED";
                break;
            }
            if( !ValidTime( t ) )
            {
                error = "bad timestamp in FRAME_MARK_NAMED";
                break;
            }
            // Frame sets are global in Tracy, so "Physics" from the server and from a
            // client stay apart as "Server: Physics" and "Client 1: Physics".
            const char* name = PrefixedNameFor( id, unknown );
            const int64_t time = Map( t );
            if( connected ) emit::FrameMark( name, time );
            break;
        }
        case OpPlotValue:
        {
            uint16_t id = 0;
            double t = 0, value = 0;
            if( !r.Read( id ) || !r.Read( t ) || !r.Read( value ) )
            {
                error = "truncated PLOT_VALUE";
                break;
            }
            if( !ValidTime( t ) )
            {
                error = "bad timestamp in PLOT_VALUE";
                break;
            }
            const char* name = PrefixedNameFor( id, unknown );  // plots are global too
            const int64_t time = Map( t );
            // A NaN or infinite sample (0/0 in game code) would wreck the plot's scale.
            if( connected && std::isfinite( value ) ) emit::Plot( name, time, value );
            break;
        }
        default:
            error = "unknown opcode";
            break;
        }
        if( !error ) events++;
    }

    const bool final = ( header.flags & FlagFinal ) != 0;
    if( error || final ) CloseOpenZones();
    if( final )
    {
        m_bound = false;
        if( !m_quiet ) std::printf( "[%s] session %08" PRIx32 " ended\n", m_label.c_str(), header.session );
    }

    {
        std::lock_guard<std::mutex> lock( m_statsLock );
        m_stats.batches++;
        m_stats.events += events;
        m_stats.bytes += body.size();
        m_stats.gaps += gaps;
        if( error ) m_stats.malformed++;
        if( unknown ) m_stats.unknownIds++;
    }

    if( m_verbose )
    {
        std::printf( "[%s] batch seq=%" PRIu32 " %zu bytes, %" PRIu64 " events%s%s\n", m_label.c_str(),
                     header.sequence, body.size(), events, gaps ? " (after a gap)" : "", connected ? "" : " (no viewer)" );
    }

    IngestResult result;
    result.haveOffset = m_haveOffset;
    result.offsetNs = m_offsetNs;
    if( error )
    {
        char text[96];
        std::snprintf( text, sizeof( text ), "%s at byte %zu", error, kHeaderSize + r.Offset() );
        if( !m_quiet ) std::printf( "[%s] rejected batch: %s\n", m_label.c_str(), text );
        result.status = 400;
        result.body = text;
    }
    else if( unknown )
    {
        // Some event named an id this session never registered (its registration was
        // in a lost batch, or the bridge restarted). Ask Luau to send its names again.
        result.status = 200;
        result.body = "resync";
    }
    return result;
}

// ---------------------------------------------------------------- Router

Router::Router( Interner& interner, SrcLocCache& srclocs, bool verbose, bool quiet )
    : m_interner( interner ), m_srclocs( srclocs ), m_verbose( verbose ), m_quiet( quiet )
{
}

Lane* Router::Route( const BatchHeader& header, int64_t now, std::string_view trackName )
{
    std::lock_guard<std::mutex> lock( m_lock );

    if( m_ended.count( header.session ) ) return nullptr;

    const bool named = header.source == SourceNamed;
    const std::string wantedName = named ? std::string( trackName.empty() ? "Imported" : trackName ) : std::string();

    Lane* lane = nullptr;
    if( auto it = m_bySession.find( header.session ); it != m_bySession.end() )
    {
        lane = it->second;
    }
    else
    {
        int sameSource = 0;
        for( auto& candidate : m_lanes )
        {
            if( candidate->Source() != header.source ) continue;
            // Named tracks are only reused by a session with the same name.
            if( named && candidate->ShortLabel() != wantedName ) continue;
            sameSource++;
            if( !lane && ( candidate->released || now - candidate->lastTouchNs > kIdleNs ) ) lane = candidate.get();
        }
        if( lane )
        {
            m_bySession.erase( lane->boundSession );
        }
        else
        {
            // "Server", "Client 3" or an imported thread's name. A second VM of the same
            // kind at once (an Actor's VM, say) becomes "Server #2".
            std::string shortLabel = named                          ? wantedName
                                     : header.source == SourceServer ? "Server"
                                                                     : "Client " + std::to_string( header.source );
            if( sameSource > 0 ) shortLabel += " #" + std::to_string( sameSource + 1 );
            const std::string label = named ? shortLabel : "Roblox " + shortLabel;
            // The first server track (or first named track, in an import) owns Tracy's
            // default frame set, so the frame graph and FPS stats work with no setup.
            const bool primary = !m_defaultFrameSetTaken && ( header.source == SourceServer || named );
            if( primary ) m_defaultFrameSetTaken = true;
            const char* frameName = primary ? nullptr : m_interner.Intern( shortLabel );
            const int32_t groupHint = named ? 1000 + m_namedTracks++ : int32_t( header.source );
            m_lanes.push_back( std::make_unique<Lane>( header.source, label, shortLabel, frameName, groupHint, m_interner,
                                                       m_srclocs, m_verbose, m_quiet ) );
            lane = m_lanes.back().get();
            if( !m_quiet ) std::printf( "[%s] new track\n", label.c_str() );
        }
        m_bySession[header.session] = lane;
        lane->boundSession = header.session;
    }

    lane->lastTouchNs = now;
    lane->released = false;
    if( header.flags & FlagFinal )
    {
        m_bySession.erase( header.session );
        lane->released = true;
        if( m_ended.size() >= 4096 ) m_ended.clear();
        m_ended.insert( header.session );
    }
    return lane;
}

std::string Router::StatusJson( bool viewerConnected )
{
    std::lock_guard<std::mutex> lock( m_lock );
    const int64_t now = emit::Now();
    std::string out = "{\"name\":\"tracy-roblox-bridge\",\"version\":\"" BRIDGE_VERSION "\",\"protocol\":";
    out += std::to_string( kProtocolVersion );
    out += ",\"tracy\":\"" + std::to_string( tracy::Version::Major ) + "." + std::to_string( tracy::Version::Minor ) +
           "." + std::to_string( tracy::Version::Patch ) + "\"";
    out += ",\"viewerConnected\":";
    out += viewerConnected ? "true" : "false";
    out += ",\"tracks\":[";
    bool first = true;
    for( auto& lane : m_lanes )
    {
        const LaneStats s = lane->Stats();
        if( !first ) out += ',';
        first = false;
        char session[16];
        std::snprintf( session, sizeof( session ), "%08" PRIx32, lane->boundSession );
        out += "{\"label\":";
        AppendJsonString( out, lane->Label() );
        out += ",\"source\":" + std::to_string( lane->Source() );
        out += ",\"session\":\"";
        out += session;
        out += "\",\"active\":";
        out += ( !lane->released && now - lane->lastTouchNs <= kIdleNs ) ? "true" : "false";
        out += ",\"batches\":" + std::to_string( s.batches );
        out += ",\"events\":" + std::to_string( s.events );
        out += ",\"bytes\":" + std::to_string( s.bytes );
        out += ",\"gaps\":" + std::to_string( s.gaps );
        out += ",\"droppedEnds\":" + std::to_string( s.droppedEnds );
        out += ",\"malformed\":" + std::to_string( s.malformed );
        out += ",\"unknownIds\":" + std::to_string( s.unknownIds );
        out += '}';
    }
    out += "]}";
    return out;
}

std::string Router::ProblemReport()
{
    std::lock_guard<std::mutex> lock( m_lock );
    std::string out;
    for( auto& lane : m_lanes )
    {
        const LaneStats s = lane->Stats();
        if( !s.gaps && !s.droppedEnds && !s.unknownIds && !s.malformed ) continue;
        char line[256];
        std::snprintf( line, sizeof( line ),
                       "  %s: %llu gaps, %llu zone ends without a zone, %llu batches with unknown names, %llu "
                       "malformed\n",
                       lane->Label().c_str(), (unsigned long long)s.gaps, (unsigned long long)s.droppedEnds,
                       (unsigned long long)s.unknownIds, (unsigned long long)s.malformed );
        out += line;
    }
    return out;
}

void Router::Shutdown()
{
    std::lock_guard<std::mutex> lock( m_lock );
    m_bySession.clear();
    m_lanes.clear();  // each lane closes its open zones, then its thread exits
}

// ---------------------------------------------------------------- request

RequestOutcome IngestRequest( Router& router, const std::string& body, int64_t nowNs )
{
    RequestOutcome out;
    if( body.empty() )
    {
        out.status = 400;
        out.text = "empty request";
        return out;
    }
    Reader reader( body.data(), body.size() );
    ClockRef ref;
    size_t batches = 0;
    while( !reader.Empty() )
    {
        const size_t start = reader.Offset();
        BatchHeader header;
        const char* error = nullptr;
        if( !ParseHeader( reader, header, error ) )
        {
            if( batches == 0 )
            {
                out.status = 400;
                out.text = error;
                return out;
            }
            out.text += "error - ";
            out.text += error;
            out.text += '\n';
            break;
        }
        reader.Skip( header.length );
        batches++;

        const std::string_view trackName =
            header.source == SourceNamed ? LeadingTrackName( body.data() + start + kHeaderSize, header.length )
                                         : std::string_view();
        Lane* lane = router.Route( header, nowNs, trackName );
        if( !lane ) continue;  // late batch from a session that already ended
        IngestResult result = lane->Submit( body.substr( start, kHeaderSize + header.length ), header, ref, nowNs );
        // The first server batch is the clock reference for the clients it relayed.
        if( header.source == SourceServer && !ref.valid && result.haveOffset )
        {
            ref.valid = true;
            ref.offsetNs = result.offsetNs;
        }
        char session[16];
        std::snprintf( session, sizeof( session ), "%08x", unsigned( header.session ) );
        if( result.status == 200 )
        {
            out.text += "resync ";
            out.text += session;
            out.text += '\n';
        }
        else if( result.status == 400 )
        {
            out.text += "error ";
            out.text += session;
            out.text += ' ';
            out.text += result.body;
            out.text += '\n';
        }
    }
    out.status = out.text.empty() ? 204 : 200;
    return out;
}

}  // namespace tlb
