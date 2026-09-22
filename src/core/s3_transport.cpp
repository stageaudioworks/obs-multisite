// SPDX-License-Identifier: GPL-3.0-or-later
#include "s3_transport.h"
#include "aws_sigv4.h"
#include "s3_list_xml.h"
#include "http_date.h"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <algorithm>

namespace multisite {

static std::once_flag g_curl_once;
static void ensure_curl() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// libcurl calls this periodically DURING a transfer, on the same thread that
// called curl_easy_perform — which is exactly the thread stop_workers() is
// trying to join. Returning non-zero aborts the transfer right there, rather
// than waiting out CURLOPT_TIMEOUT_MS.
static int curl_abort_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* cancel = static_cast<std::atomic<bool>*>(clientp);
    return (cancel && cancel->load()) ? 1 : 0;
}
static void arm_cancel(CURL* curl, std::atomic<bool>* cancel) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);   // off by default; this needs it on
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_abort_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
}

static size_t write_to_vec(void* ptr, size_t sz, size_t nm, void* ud) {
    auto* v = static_cast<std::vector<uint8_t>*>(ud);
    size_t n = sz * nm;
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    v->insert(v->end(), p, p + n);
    return n;
}

// What a response told us about itself. Only the headers worth keeping:
// cf-ray, whose suffix is the Cloudflare edge that served the request, and
// Server, which distinguishes R2 from AWS from MinIO without asking anybody.
// Date is the store's own clock, which is how a box checks its time of day.
struct HeaderCtx { std::string cf_ray; std::string server; std::string date; };
static size_t collect_headers(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* hc = static_cast<HeaderCtx*>(ud);
    const size_t n = sz * nm;
    const std::string line(ptr, n);
    std::string v;
    if (header_is(line, "cf-ray", v))      hc->cf_ray = v;
    else if (header_is(line, "server", v)) hc->server = v;
    else if (header_is(line, "date", v))   hc->date = v;
    return n;
}

struct ReadCtx { const uint8_t* data; size_t size; size_t pos; };
static size_t read_from_buf(void* dest, size_t sz, size_t nm, void* ud) {
    auto* rc = static_cast<ReadCtx*>(ud);
    size_t want = sz * nm, rem = rc->size - rc->pos;
    size_t n = std::min(want, rem);
    if (n) { std::memcpy(dest, rc->data + rc->pos, n); rc->pos += n; }
    return n;
}

// Operators paste values from dashboards, so a field may arrive with a scheme,
// a trailing slash or stray whitespace. Left as-is these produce URLs like
// "https://https://acct.r2.cloudflarestorage.com/..." and curl rejects them
// with "URL using bad/illegal format" — an error that says nothing about the
// cause. Normalise instead of failing.
static std::string clean_host(std::string v) {
    // trim whitespace
    const char* ws = " \t\r\n";
    size_t a = v.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(ws);
    v = v.substr(a, b - a + 1);
    // strip a scheme
    for (const char* p : { "https://", "http://" }) {
        const size_t n = std::strlen(p);
        if (v.size() > n && v.compare(0, n, p) == 0) { v = v.substr(n); break; }
    }
    // strip any path or trailing slashes
    const size_t slash = v.find('/');
    if (slash != std::string::npos) v = v.substr(0, slash);
    return v;
}

static std::string clean_segment(std::string v) {
    const char* ws = " \t\r\n/";
    size_t a = v.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(ws);
    return v.substr(a, b - a + 1);
}

struct S3Transport::Impl {
    S3Config cfg;

    // Add the session token to a request's signed headers, when this transport
    // holds temporary credentials. Called at EVERY signing site, which is why it
    // is a function rather than a line copied five times: a token missing from
    // one of them is a request that fails only when a brokered device happens
    // to use that verb, and that is the kind of gap nothing catches by reading.
    //
    // Signed rather than merely sent: X-Amz-Security-Token is part of the
    // credential, so SigV4 must cover it — the signer signs whatever it is
    // given and lists it in SignedHeaders.
    void add_session_token(std::map<std::string, std::string>& extra) const {
        if (!cfg.session_token.empty())
            extra["X-Amz-Security-Token"] = cfg.session_token;
    }

    // Set by cancel_pending(), read by curl_abort_cb on whichever thread is
    // mid-request. One instance, one direction: never cleared, because the
    // transport that gets cancelled is the one about to be discarded, not
    // reused.
    std::atomic<bool> cancel{false};

    // Observations from ordinary traffic. Guarded because the decoder's
    // download thread writes them while the UI thread reads them, several
    // times a second.
    mutable std::mutex obs_mtx;
    std::string last_colo;
    std::string last_server;
    // The store's clock minus ours, from the Date header — see
    // Transport::server_clock_skew_ms. Guarded by obs_mtx with the rest.
    int64_t     last_skew_ms = 0;
    // Kept apart because they answer different questions and a site only ever
    // does one of them: a main site's figure is what its upload is managing, a
    // campus's is whether it can bank a buffer. Averaging them together would
    // describe neither.
    RateMeter   down;
    RateMeter   up;

    void observe(const HeaderCtx& hc, uint64_t bytes, double seconds,
                 bool uploading) {
        std::lock_guard<std::mutex> lk(obs_mtx);
        const std::string colo = cloudflare_colo(hc.cf_ray);
        if (!colo.empty())        last_colo = colo;
        if (!hc.server.empty())   last_server = hc.server;
        // The store stamps Date from its own NTP-disciplined servers, so the
        // difference is this machine's error (to within the round trip, which
        // is milliseconds and irrelevant to a seconds-level check).
        int64_t server_ms = 0;
        if (!hc.date.empty() && parse_http_date_ms(hc.date, server_ms)) {
            const int64_t local_ms =
                (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            last_skew_ms = server_ms - local_ms;
        }
        (uploading ? up : down).add(bytes, seconds);
    }

    std::string host() const {
        const std::string ep = clean_host(cfg.endpoint_host);
        if (!ep.empty()) return ep;

        // The account-id field often receives a pasted hostname or full URL
        // rather than the bare id. If it already looks like a hostname, use it
        // as the endpoint instead of appending the R2 suffix to it.
        const std::string acct = clean_host(cfg.r2_account_id);
        if (acct.empty()) return "";      // caller reports this properly
        if (acct.find('.') != std::string::npos) return acct;
        return acct + ".r2.cloudflarestorage.com";
    }
    std::string url_for(const std::string& key) const {
        return bucket_url() + "/" + key;
    }
    // The bucket itself, with no trailing slash — what a listing addresses.
    std::string bucket_url() const {
        return std::string(cfg.use_https ? "https://" : "http://") +
               host() + "/" + clean_segment(cfg.bucket);
    }

    // x-amz-tagging value: url-encoded key=value pairs joined by &
    static std::string tag_header(const std::map<std::string, std::string>& tags) {
        std::string out;
        for (const auto& [k, v] : tags) {
            if (!out.empty()) out += "&";
            out += SigV4Signer::uri_encode(k, true) + "=" +
                   SigV4Signer::uri_encode(v, true);
        }
        return out;
    }

    PutResult do_put(const std::string& key, const std::vector<uint8_t>& body,
                     const std::string& content_type,
                     const std::map<std::string, std::string>& tags) {
        ensure_curl();
        PutResult res;
        CURL* curl = curl_easy_init();
        if (!curl) { res.error = "curl_easy_init failed"; return res; }

        std::string url = url_for(key);

        std::map<std::string, std::string> extra;
        if (!content_type.empty()) extra["Content-Type"] = content_type;
        // Retention tagging + CDN cache hint. NOTE: Cloudflare R2 rejects
        // x-amz-tagging, so the header is only sent when tags are supplied.
        std::string tg = tag_header(tags);
        if (!tg.empty()) extra["x-amz-tagging"] = tg;
        extra["Cache-Control"] = "max-age=604800";
        add_session_token(extra);

        SigV4Signer signer(cfg.access_key_id, cfg.secret_access_key,
                           cfg.region, "s3");
        auto signed_req = signer.sign("PUT", url, body, extra);

        struct curl_slist* headers = nullptr;
        for (const auto& line : signed_req.header_lines())
            headers = curl_slist_append(headers, line.c_str());

        std::vector<uint8_t> resp;
        ReadCtx rc{ body.data(), body.size(), 0 };

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_from_buf);
        curl_easy_setopt(curl, CURLOPT_READDATA, &rc);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)body.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        HeaderCtx hc;
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, collect_headers);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hc);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)cfg.connect_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)cfg.request_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        arm_cancel(curl, &cancel);

        CURLcode cc = curl_easy_perform(curl);
        if (cc == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            res.http_status = code;
            res.success = (code >= 200 && code < 300);

            // Measured from the segment uploads themselves, so a main site's
            // figure is what its own connection is actually managing during a
            // event — which is the number an operator wants when the queue
            // starts to build.
            curl_off_t start_us = 0, total_us = 0;
            curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &start_us);
            curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_us);
            const double transfer_s = total_us > start_us
                ? (double)(total_us - start_us) / 1e6 : 0.0;
            observe(hc, (uint64_t)body.size(), transfer_s, true);
            if (!res.success) {
                // 4xx (except 408/429) are permanent: bad creds, bad bucket,
                // bad request. Retrying those forever would mask a config error.
                res.retryable = !(code >= 400 && code < 500) ||
                                code == 408 || code == 429;
                std::string bodytxt(resp.begin(),
                    resp.begin() + std::min<size_t>(resp.size(), 400));
                res.error = "HTTP " + std::to_string(code) + " " + bodytxt;
            }
        } else {
            // Network-level failure → retryable (this is the outage case).
            res.retryable = true;
            res.error = curl_easy_strerror(cc);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return res;
    }

    // The read path: every manifest, every marker file and every segment a
    // satellite fetches comes through here. That is why the observations are
    // taken here rather than in a separate test — the colo and the throughput
    // shown to an operator are measured from the traffic actually carrying the
    // event, not from a synthetic probe that might take a different route.
    PutResult do_get(const std::string& key, std::vector<uint8_t>& out,
                     HeaderCtx* hdrs = nullptr, int64_t* elapsed_ms = nullptr) {
        ensure_curl();
        PutResult res;
        CURL* curl = curl_easy_init();
        if (!curl) { res.error = "curl init"; return res; }
        std::string url = url_for(key);
        SigV4Signer signer(cfg.access_key_id, cfg.secret_access_key, cfg.region, "s3");
        std::map<std::string, std::string> extra;
        add_session_token(extra);
        auto sr = signer.sign("GET", url, {}, extra);
        struct curl_slist* h = nullptr;
        for (const auto& l : sr.header_lines()) h = curl_slist_append(h, l.c_str());
        HeaderCtx hc;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, collect_headers);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hc);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)cfg.connect_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)cfg.request_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        arm_cancel(curl, &cancel);
        CURLcode cc = curl_easy_perform(curl);
        if (cc == CURLE_OK) {
            long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            res.http_status = code;
            res.success = (code >= 200 && code < 300);
            if (!res.success) res.error = "HTTP " + std::to_string(code);

            // curl's own figure for the transfer, which excludes name
            // resolution and the handshake — so this is the link's throughput
            // rather than a round trip's.
            curl_off_t t_us = 0;
            curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &t_us);
            curl_off_t total_us = 0;
            curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_us);
            const double transfer_s = total_us > t_us
                ? (double)(total_us - t_us) / 1e6 : 0.0;
            if (elapsed_ms) *elapsed_ms = (int64_t)(total_us / 1000);
            observe(hc, (uint64_t)out.size(), transfer_s, false);
        } else {
            res.error = curl_easy_strerror(cc);
            if (elapsed_ms) *elapsed_ms = 0;
        }
        if (hdrs) *hdrs = hc;
        curl_slist_free_all(h);
        curl_easy_cleanup(curl);
        return res;
    }

    // Delete one object. A signed DELETE with no body; a 204 (any 2xx) means
    // gone, and a 404 is treated as success because the caller wanted it gone
    // either way.
    DeleteResult do_remove(const std::string& key) {
        ensure_curl();
        DeleteResult res;
        CURL* curl = curl_easy_init();
        if (!curl) { res.error = "curl_easy_init failed"; return res; }

        std::string url = url_for(key);
        SigV4Signer signer(cfg.access_key_id, cfg.secret_access_key,
                           cfg.region, "s3");
        std::map<std::string, std::string> extra;
        add_session_token(extra);
        auto sr = signer.sign("DELETE", url, {}, extra);
        struct curl_slist* h = nullptr;
        for (const auto& l : sr.header_lines()) h = curl_slist_append(h, l.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)cfg.connect_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)cfg.request_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        arm_cancel(curl, &cancel);

        CURLcode cc = curl_easy_perform(curl);
        if (cc == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            res.http_status = code;
            // 204 No Content is the normal answer; any 2xx works, and 404 is
            // already-gone, which is the outcome the caller asked for.
            res.success = (code >= 200 && code < 300) || code == 404;
            res.retryable = code == 500 || code == 503 || code == 502 ||
                            code == 408 || code == 429;
            if (!res.success)
                res.error = "HTTP " + std::to_string(code);
        } else {
            res.retryable = true;
            res.error = curl_easy_strerror(cc);
        }

        curl_slist_free_all(h);
        curl_easy_cleanup(curl);
        return res;
    }
};

S3Transport::S3Transport(S3Config cfg) : d(std::make_unique<Impl>()) {
    d->cfg = std::move(cfg);
}

// The base URL actually in use, for logging. Contains no credentials.
std::string S3Transport::base_url() const {
    const std::string h = d->host();
    if (h.empty()) return "<no endpoint configured>";
    return std::string(d->cfg.use_https ? "https://" : "http://") + h + "/" +
           clean_segment(d->cfg.bucket);
}
S3Transport::~S3Transport() = default;

void S3Transport::cancel_pending() { d->cancel = true; }
bool S3Transport::last_request_cancelled() const { return d->cancel.load(); }
void S3Transport::resume_pending() { d->cancel = false; }

std::string S3Transport::host() const { return d->host(); }

std::string S3Transport::last_colo() const {
    std::lock_guard<std::mutex> lk(d->obs_mtx);
    return d->last_colo;
}
std::string S3Transport::last_server() const {
    std::lock_guard<std::mutex> lk(d->obs_mtx);
    return d->last_server;
}
int64_t S3Transport::server_clock_skew_ms() const {
    std::lock_guard<std::mutex> lk(d->obs_mtx);
    return d->last_skew_ms;
}
double S3Transport::observed_upload_bytes_per_s() const {
    std::lock_guard<std::mutex> lk(d->obs_mtx);
    return d->up.bytes_per_s();
}
uint64_t S3Transport::upload_samples() const {
    std::lock_guard<std::mutex> lk(d->obs_mtx);
    return d->up.samples();
}
double S3Transport::observed_download_bytes_per_s() const {
    std::lock_guard<std::mutex> lk(d->obs_mtx);
    return d->down.bytes_per_s();
}
uint64_t S3Transport::download_samples() const {
    std::lock_guard<std::mutex> lk(d->obs_mtx);
    return d->down.samples();
}

StorageProbe S3Transport::probe(const std::string& key) {
    StorageProbe p;
    p.endpoint = d->host();
    if (p.endpoint.empty()) {
        p.error = "no endpoint or account id has been set";
        return p;
    }

    std::vector<uint8_t> body;
    HeaderCtx hc;
    int64_t ms = 0;
    const PutResult r = d->do_get(key, body, &hc, &ms);

    p.http_status   = r.http_status;
    p.round_trip_ms = ms;
    p.colo          = cloudflare_colo(hc.cf_ray);
    p.server        = hc.server;
    // An HTTP status of any kind means the endpoint is there and answering,
    // which is a different fact from whether our key may read the object —
    // and they send an operator to different places. DNS or a dead link gives
    // no status at all.
    p.reachable = r.http_status > 0;
    p.readable  = r.success;

    if (r.success) return p;
    if (!p.reachable) {
        p.error = r.error.empty() ? "the endpoint could not be reached"
                                  : r.error;
    } else if (r.http_status == 404) {
        p.error = "reached the bucket, but " + key + " is not there — check "
                  "the feed name, or nothing has been broadcast to it yet";
    } else if (r.http_status == 403 || r.http_status == 401) {
        p.error = "reached the bucket, but the key was refused — check the "
                  "access key, the secret and the bucket name";
    } else {
        p.error = "reached the bucket, but it answered HTTP " +
                  std::to_string(r.http_status);
    }
    return p;
}

PutResult S3Transport::put(const std::string& key,
                           const std::vector<uint8_t>& body,
                           const std::string& content_type,
                           const std::map<std::string, std::string>& tags) {
    return d->do_put(key, body, content_type, tags);
}

GetResult S3Transport::get(const std::string& key) {
    GetResult out;
    std::vector<uint8_t> body;
    PutResult r = d->do_get(key, body);
    out.success     = r.success;
    out.http_status = r.http_status;
    out.error       = r.error;
    // 4xx (other than 408/429) are permanent: wrong key, bad creds, no bucket.
    out.retryable   = !(r.http_status >= 400 && r.http_status < 500) ||
                      r.http_status == 408 || r.http_status == 429;
    out.body        = std::move(body);
    return out;
}

DeleteResult S3Transport::remove(const std::string& key) {
    return d->do_remove(key);
}

ListResult S3Transport::list(const std::string& prefix,
                             const std::string& delimiter,
                             const std::string& continuation_token,
                             int max_keys) {
    ensure_curl();
    ListResult out;

    CURL* curl = curl_easy_init();
    if (!curl) { out.error = "curl_easy_init failed"; return out; }

    // Values are percent-encoded here (a continuation token is base64 and
    // routinely contains '+', '/' and '='; a room name may contain anything an
    // operator typed). The signer decodes before canonicalising, so the
    // signature is computed over the same values the store will parse out.
    auto q = [](const std::string& k, const std::string& v) {
        return "&" + k + "=" + SigV4Signer::uri_encode(v, true);
    };
    std::string url = d->bucket_url() + "/?list-type=2";
    if (!prefix.empty())             url += q("prefix", prefix);
    if (!delimiter.empty())          url += q("delimiter", delimiter);
    if (!continuation_token.empty()) url += q("continuation-token", continuation_token);
    if (max_keys > 0)                url += q("max-keys", std::to_string(max_keys));

    SigV4Signer signer(d->cfg.access_key_id, d->cfg.secret_access_key,
                       d->cfg.region, "s3");
    std::map<std::string, std::string> extra;
    d->add_session_token(extra);
    auto sr = signer.sign("GET", url, {}, extra);

    struct curl_slist* h = nullptr;
    for (const auto& l : sr.header_lines()) h = curl_slist_append(h, l.c_str());

    std::vector<uint8_t> body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)d->cfg.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)d->cfg.request_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    arm_cancel(curl, &d->cancel);

    CURLcode cc = curl_easy_perform(curl);
    if (cc != CURLE_OK) {
        out.retryable = true;                  // network-level: the outage case
        out.error = curl_easy_strerror(cc);
        curl_slist_free_all(h);
        curl_easy_cleanup(curl);
        return out;
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);

    out.http_status = code;
    out.retryable   = !(code >= 400 && code < 500) || code == 408 || code == 429;

    const std::string xml(body.begin(), body.end());
    if (code < 200 || code >= 300) {
        // Parse the error document for its Code/Message, which say far more
        // than the status does.
        ListResult err;
        parse_list_objects_v2(xml, err);
        out.error = "HTTP " + std::to_string(code);
        if (!err.error.empty()) out.error += " " + err.error;
        // The failure an operator will actually hit: a token scoped to objects
        // rather than the bucket. An empty event list would look like "no
        // recordings" instead of "the key cannot list".
        if (code == 403)
            out.error += " — listing requires the s3:ListBucket permission; "
                         "this key appears to be object-scoped";
        return out;
    }

    if (!parse_list_objects_v2(xml, out)) {
        out.retryable = false;
        if (out.error.empty()) out.error = "unparseable listing response";
        return out;
    }

    out.success = true;
    return out;
}

int64_t S3Transport::object_size(const std::string& key) {
    ensure_curl();
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    std::string url = d->url_for(key);
    SigV4Signer signer(d->cfg.access_key_id, d->cfg.secret_access_key,
                       d->cfg.region, "s3");
    std::map<std::string, std::string> extra;
    d->add_session_token(extra);
    auto sr = signer.sign("HEAD", url, {}, extra);
    struct curl_slist* h = nullptr;
    for (const auto& l : sr.header_lines()) h = curl_slist_append(h, l.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);          // HEAD
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)d->cfg.request_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    arm_cancel(curl, &d->cancel);

    int64_t size = -1;
    if (curl_easy_perform(curl) == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code >= 200 && code < 300) {
            curl_off_t len = -1;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
            size = (int64_t)len;
        }
    }
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    return size;
}

std::string S3Transport::self_test() {
    const std::string key = "_multisite_probe.txt";
    std::string payload = "obs-multisite connectivity probe";
    std::vector<uint8_t> body(payload.begin(), payload.end());

    auto p = d->do_put(key, body, "text/plain", { {"MultisiteExpiry", "7d"} });
    if (!p.success) return "write failed: " + p.error;

    std::vector<uint8_t> got;
    auto g = d->do_get(key, got);
    if (!g.success) return "read-back failed: " + g.error;
    if (got.size() != body.size())
        return "read-back mismatch (wrote " + std::to_string(body.size()) +
               " bytes, read " + std::to_string(got.size()) + ")";
    return "";
}

} // namespace multisite
