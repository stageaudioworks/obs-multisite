// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// null_transport.h — a Transport that keeps nothing anywhere: every PUT
// reports instant success without ever touching a network or a disk beyond
// what the caller already durably wrote itself.
//
// Exists for one thing: LAN-only delivery (PROJECT-SCOPE.md §8.7). An
// operator who has no cloud bucket at all, or who simply doesn't want one for
// this event, still needs Session's entire pipeline — spool, retry-uploader,
// manifest publishing, the three LAN hooks — to behave exactly as it always
// has. Session has no notion of "delivery disabled"; it only knows whether
// its Transport's put() succeeded. Handing it this instead of an S3Transport
// makes every put() succeed on the first attempt, which is indistinguishable
// to Session from an extremely fast, extremely reliable bucket — segments
// confirm immediately, the LAN hooks fire on schedule, and nothing is ever
// retried because nothing ever fails.
//
#include "transport.h"

#include <map>
#include <mutex>

namespace multisite {

class NullTransport : public Transport {
public:
    PutResult put(const std::string& key,
                  const std::vector<uint8_t>& body,
                  const std::string& /*content_type*/,
                  const std::map<std::string, std::string>& /*tags*/) override {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_sizes[key] = (int64_t)body.size();
        PutResult r;
        r.success = true;
        r.http_status = 200;
        return r;
    }

    // Echoes back the size just "put", so RetryUploader's post-success verify
    // step (UploaderConfig::verify_first_n) sees a match instead of reporting
    // a false "object not found after a successful PUT" for the first few
    // segments of every LAN-only event.
    int64_t object_size(const std::string& key) override {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_sizes.find(key);
        return it == m_sizes.end() ? -1 : it->second;
    }

    // get()/list()/remove() keep the base class's "not implemented" behaviour:
    // nothing on the encoder side ever calls them — reads are a decoder's job,
    // and a decoder has no reason to hold a NullTransport.

private:
    std::mutex m_mtx;
    std::map<std::string, int64_t> m_sizes;
};

} // namespace multisite
