// Replays Tracy.luau batches into Tracy.
//
// A Lane is one OS thread, which Tracy shows as one track ("Roblox Server").
// Each Luau VM run (a session) is bound to a lane. A new session reuses an idle lane
// of the same kind, so repeated playtests stay on one track.
#pragma once

#include "Protocol.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tracy
{
struct SourceLocationData;
}

namespace tlb
{

// The viewer asks for strings by pointer, at any time, so every string handed to
// Tracy lives until the process exits. Same text -> same pointer, which also keeps
// frame sets and plots with the same name together.
class Interner
{
public:
    const char* Intern( std::string_view text );

private:
    std::mutex m_lock;
    std::unordered_set<std::string> m_strings;  // node-based: addresses never move
};

// Source locations are what Tracy groups zone statistics by. One per
// (name, track label, color). Never freed: Tracy can still read them while the
// process exits.
class SrcLocCache
{
public:
    const tracy::SourceLocationData* Get( const char* name, const char* file, uint32_t color );

private:
    std::mutex m_lock;
    std::map<std::tuple<const char*, const char*, uint32_t>, const tracy::SourceLocationData*> m_locs;
};

struct IngestResult
{
    int status = 204;  // 204 ok, 200 + "resync", 400 + reason
    std::string body;
    // The session's clock offset after this batch. A server batch's offset is the
    // reference for the client batches it relayed in the same request.
    bool haveOffset = false;
    int64_t offsetNs = 0;
};

// Clock reference for relayed client batches: the relaying server session's offset.
struct ClockRef
{
    bool valid = false;
    int64_t offsetNs = 0;
};

struct LaneStats
{
    uint64_t batches = 0;
    uint64_t events = 0;
    uint64_t bytes = 0;
    uint64_t gaps = 0;         // batches lost between Luau and the bridge
    uint64_t droppedEnds = 0;  // ZoneEnd with no open zone
    uint64_t malformed = 0;
    uint64_t unknownIds = 0;  // events naming a string id never registered
};

class Lane
{
public:
    // label: the track name ("Roblox Client 1"). shortLabel prefixes this track's plot
    // and named frame-set names ("Client 1: Lua heap"), which Tracy keeps global.
    // groupHint orders tracks in the viewer (lower first).
    Lane( uint8_t source, std::string label, std::string shortLabel, const char* frameName, int32_t groupHint,
          Interner& interner, SrcLocCache& srclocs, bool verbose, bool quiet );
    ~Lane();

    Lane( const Lane& ) = delete;
    Lane& operator=( const Lane& ) = delete;

    // Called from HTTP threads. Blocks until this lane's thread has replayed the batch,
    // so the HTTP response can report problems back to Luau. `body` is one batch,
    // header included. `nowNs` is the request's arrival on Tracy's clock (when
    // converting a recording, the recorded arrival on a virtual clock).
    IngestResult Submit( std::string body, const BatchHeader& header, ClockRef ref, int64_t nowNs );

    uint8_t Source() const { return m_source; }
    const std::string& Label() const { return m_label; }
    const std::string& ShortLabel() const { return m_shortLabel; }
    LaneStats Stats();

    // Router bookkeeping, only touched under the router's lock.
    uint32_t boundSession = 0;
    int64_t lastTouchNs = 0;
    bool released = true;

private:
    struct Job
    {
        std::string body;
        BatchHeader header;
        ClockRef ref;
        int64_t nowNs = 0;
        std::promise<IngestResult> done;
    };

    void Run();
    IngestResult Replay( const std::string& body, const BatchHeader& header, ClockRef ref, int64_t nowNs );
    void Rebind( const BatchHeader& header );
    void SetName( uint16_t id, const char* name );
    const char* NameFor( uint16_t id, bool& unknown );
    const char* PrefixedNameFor( uint16_t id, bool& unknown );
    const tracy::SourceLocationData* SrcLocFor( uint16_t id, bool& unknown );
    const tracy::SourceLocationData* ColoredSrcLocFor( uint16_t id, uint32_t color, bool& unknown );
    int64_t Map( double luauSeconds );
    void ZoneBegin( const tracy::SourceLocationData* srcloc, int64_t time );
    void ZoneEnd( int64_t time );
    void CloseOpenZones();

    const uint8_t m_source;
    const std::string m_label;
    const std::string m_shortLabel;
    const char* const m_labelInterned;
    const char* const m_frameName;
    const int32_t m_groupHint;
    Interner& m_interner;
    SrcLocCache& m_srclocs;
    const bool m_verbose;
    const bool m_quiet;  // no session/track log lines (converting a recording)

    std::mutex m_jobLock;
    std::condition_variable m_jobReady;
    std::deque<std::shared_ptr<Job>> m_jobs;
    bool m_stop = false;

    std::mutex m_statsLock;
    LaneStats m_stats;

    // Replay state: only the lane thread touches these.
    bool m_bound = false;
    uint32_t m_session = 0;
    uint32_t m_expectedSeq = 0;
    bool m_haveOffset = false;
    int64_t m_offsetNs = 0;
    int64_t m_lastTime = 0;
    std::vector<const char*> m_names;
    std::vector<const tracy::SourceLocationData*> m_zoneLocs;
    std::vector<const char*> m_prefixedNames;  // plots and named frame sets
    std::unordered_map<uint64_t, const tracy::SourceLocationData*> m_colorLocs;  // (id << 32) | color
    // One entry per zone Luau has open: its start, and the viewer connection it was
    // sent to (kNotSent if none). A zone's end goes only to the connection that got
    // its begin.
    struct OpenZone
    {
        int64_t begin;
        uint64_t sentTo;
    };
    std::vector<OpenZone> m_zoneStack;

    std::thread m_thread;  // last, so it starts after everything above exists
};

class Router
{
public:
    Router( Interner& interner, SrcLocCache& srclocs, bool verbose, bool quiet = false );

    // nullptr: the session already sent its final batch; ignore the stragglers.
    // trackName: the TRACK_NAME of a SourceNamed batch (empty otherwise).
    Lane* Route( const BatchHeader& header, int64_t nowNs, std::string_view trackName = {} );
    std::string StatusJson( bool viewerConnected );
    // One line per track that had gaps, dropped zone ends, unknown ids or bad batches.
    std::string ProblemReport();
    // Ends every track (closing open zones) and stops the lane threads.
    void Shutdown();

private:
    Interner& m_interner;
    SrcLocCache& m_srclocs;
    const bool m_verbose;
    const bool m_quiet;

    std::mutex m_lock;
    std::vector<std::unique_ptr<Lane>> m_lanes;
    std::unordered_map<uint32_t, Lane*> m_bySession;
    std::unordered_set<uint32_t> m_ended;
    bool m_defaultFrameSetTaken = false;
    int32_t m_namedTracks = 0;
};

// One HTTP request: one or more batches back to back, the server's own first, then
// any it relayed from clients (docs/PROTOCOL.md).
struct RequestOutcome
{
    int status = 204;  // 204 all fine, 200 + notes, 400 + reason
    std::string text;
};
RequestOutcome IngestRequest( Router& router, const std::string& body, int64_t nowNs );

}  // namespace tlb
