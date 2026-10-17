#include "pch.h"
#include "hook.h"
#include "features.h"
#include "featureproto.h"
#include "serverpackets.h"
#include "v83_offsets.h"
#include "wvs/packet.h"
#include <cstdarg>
#include <cstdio>
#include <string>

// The login feature handshake (BotStory server docs/CLIENT_FEATURE_HANDSHAKE.md): after the first SET_FIELD of each
// connection the client REPORTs every module (id, title, version, flags, state); the server answers a POLICY of
// allow | deny | require per module, and the client carries it out and says what it did (APPLIED):
//   deny          a LIVE module goes inert now (features::SetActive); any other one stays on until the game closes and
//                 is kept off from the next start (config.ini [features] inert=)
//   allow/require a veto kept in inert= is lifted, and an inert LIVE module is active again
// The player's own switches stay as they are: an allowed module the player turned off stays off (the server tells the
// player when it requires one). Core modules never change. A server without the handshake never answers, so the
// client runs exactly on its own switches.

using namespace v83::handshake;
using namespace featureproto;

namespace {

bool g_bReported = false; // on this connection

void hs_log(const char* fmt, ...) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, "kaentake_diag.txt", "a") != 0 || !fp) {
        return;
    }
    fputs("[handshake] ", fp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

std::vector<Feature> CurrentFeatures() {
    std::vector<Feature> v;
    for (int i = 0; i < static_cast<int>(FeatureId::count); ++i) {
        auto id = static_cast<FeatureId>(i);
        Feature f;
        f.id = features::Name(id);
        f.title = features::Title(id);
        f.version = features::Version(id);
        f.flags = features::Flags(id);
        f.state = (features::IsAttached(id) ? STATE_ATTACHED : 0) | (features::IsActive(id) ? STATE_ACTIVE : 0)
                | (features::IsVetoed(id) ? STATE_VETOED : 0);
        v.push_back(std::move(f));
    }
    return v;
}

void SendReport(ClientOp op) {
    std::vector<unsigned char> body = EncodeReport(op, kProtocol, kDllBuild, CurrentFeatures());
    COutPacket o(kSendOpcode);
    o.EncodeBuffer(body.data(), body.size());
    serverpackets::Send(o);
}

void Apply(const Policy& policy) {
    std::string inertNow, offNextStart, activeNow, lifted;
    auto add = [](std::string& s, const char* sId) { s += (s.empty() ? "" : ",") + std::string(sId); };
    for (const auto& a : policy.answers) {
        FeatureId id = features::Find(a.id.c_str());
        if (id == FeatureId::none || (features::Flags(id) & FEATURE_CORE)) {
            continue; // a module this build does not have, or one that is never switched
        }
        const char* sId = features::Name(id);
        if (a.decision == DECISION_DENY) {
            if (!features::IsActive(id)) {
                continue; // off, inert or vetoed already
            }
            if (features::SetActive(id, false)) {
                add(inertNow, sId);
            } else if (features::SetVetoed(id, true)) {
                add(offNextStart, sId); // not LIVE (or it shares bytes): it runs until the game closes
            }
            continue;
        }
        if (features::IsVetoed(id) && features::SetVetoed(id, false)) {
            add(lifted, sId);
        }
        if (features::IsAttached(id) && !features::IsActive(id) && features::SetActive(id, true)) {
            add(activeNow, sId);
        }
    }
    hs_log("POLICY protocol %d, %u answers: inert now [%s], off from the next start [%s], active again [%s], "
           "veto lifted [%s]", policy.protocol, static_cast<unsigned int>(policy.answers.size()), inertNow.c_str(),
           offNextStart.c_str(), activeNow.c_str(), lifted.c_str());
    SendReport(OP_APPLIED);
}

void OnPolicyPacket(const unsigned char* p, size_t n) {
    Policy policy;
    if (!ReadPolicy(p, n, policy)) {
        hs_log("bad POLICY (%u bytes)", static_cast<unsigned int>(n));
        return;
    }
    Apply(policy);
}

// SEH must not share a frame with C++ objects that unwind, so the guarded call is its own function.
void OnPolicyRecv(const unsigned char* p, size_t n) {
    __try {
        OnPolicyPacket(p, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hs_log("exception while reading a POLICY");
    }
}

void OnSetField() {
    if (!g_bReported) {
        g_bReported = true;
        SendReport(OP_REPORT);
        hs_log("REPORT sent (protocol %d, build %d)", kProtocol, kDllBuild);
    }
}

}  // namespace


// Called by bypass.cpp's CClientSocket::Connect hook: a new connection has not reported yet.
void FeatureHandshakeOnConnect() {
    g_bReported = false;
}

// server_packets' attach function calls this once its ProcessPacket hook is in.
void AttachFeatureHandshake() {
    serverpackets::OnRecv(kRecvOpcode, OnPolicyRecv);
    serverpackets::OnSetField(OnSetField);
}
