#include "low_latency/recovery.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace low_latency {
namespace {
std::uint32_t read32(std::span<const std::uint8_t> b, std::size_t at) {
    return std::uint32_t{b[at]} | (std::uint32_t{b[at+1]} << 8) |
           (std::uint32_t{b[at+2]} << 16) | (std::uint32_t{b[at+3]} << 24);
}
void put(std::span<std::uint8_t> b, std::size_t at, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) { b[at+i] = static_cast<std::uint8_t>(value >> (i*8)); }
}
std::uint32_t distance(std::uint32_t first, std::uint32_t last) {
    return last >= first ? last-first : UINT32_MAX-first+last;
}
std::uint32_t next(std::uint32_t s, std::uint32_t n = 1) {
    return static_cast<std::uint32_t>((std::uint64_t{s}-1+n) % UINT32_MAX+1);
}
[[noreturn]] void os_fail(const char* op) { throw std::system_error(errno, std::generic_category(), op); }

class Session {
public:
    explicit Session(const std::string& endpoint, std::uint8_t unit) : unit_(unit) {
        const auto colon = endpoint.find(':');
        if (colon == std::string::npos) { throw std::invalid_argument("Recovery endpoint must be IPv4:port"); }
        sockaddr_in address{}; address.sin_family = AF_INET;
        const auto port_text = endpoint.substr(colon+1);
        if (port_text.empty() || port_text.find_first_not_of("0123456789") != std::string::npos) {
            throw std::invalid_argument("Invalid recovery port");
        }
        const auto port = std::stoul(port_text);
        if (port == 0 || port > 65535 || inet_pton(AF_INET, endpoint.substr(0, colon).c_str(), &address.sin_addr) != 1) {
            throw std::invalid_argument("Recovery endpoint must be IPv4:port");
        }
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd_ < 0) { os_fail("recovery socket"); }
        try {
            if (connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
                if (errno != EINPROGRESS) { os_fail("recovery connect"); }
                pollfd p{fd_, POLLOUT, 0};
                if (poll(&p, 1, 2000) <= 0) { throw std::runtime_error("Recovery connect timeout"); }
                int error = 0; socklen_t length = sizeof(error);
                if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0) { os_fail("recovery connect status"); }
                if (error) { throw std::system_error(error, std::generic_category(), "recovery connect"); }
            }
            std::array<std::uint8_t, 22> login{}; login.fill(' '); login[0] = 22; login[1] = 1;
            const auto field = [&](const char* env, std::size_t offset, std::size_t size) {
                const char* value = std::getenv(env);
                if (!value || std::strlen(value) > size) { throw std::invalid_argument(std::string("Set ")+env+" to a valid PITCH login field"); }
                std::copy_n(value, std::strlen(value), login.begin()+static_cast<std::ptrdiff_t>(offset));
            };
            field("PITCH_SESSION", 2, 4); field("PITCH_USERNAME", 6, 4); field("PITCH_PASSWORD", 12, 10);
            send_message(login);
            last_rx_ = last_tx_ = monotonic_ns();
        } catch (...) { close(fd_); throw; }
    }
    ~Session() { close(fd_); }
    bool logged_in = false;
    bool writable() const { return out_size_ == 0; }
    void send_message(std::span<const std::uint8_t> message) {
        if (out_size_) { throw std::runtime_error("Recovery control output still pending"); }
        out_.fill(0); put(out_, 0, message.size()+8, 2); out_[2] = message.empty() ? 0 : 1;
        std::copy(message.begin(), message.end(), out_.begin()+8);
        out_size_ = message.size()+8; out_at_ = 0; flush();
    }
    void flush() {
        if (!out_size_) { return; }
        const auto n = send(fd_, out_.data()+out_at_, out_size_-out_at_, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { return; }
            os_fail("recovery send");
        }
        out_at_ += static_cast<std::size_t>(n);
        if (out_at_ == out_size_) { out_size_ = 0; last_tx_ = monotonic_ns(); }
    }
    template<class F> void tick(std::uint64_t now, F&& handle) {
        flush();
        // Bound each service pass so a busy TCP stream cannot starve multicast.
        for (unsigned pass = 0; pass < 4; ++pass) {
            const auto n = recv(fd_, input_.data()+used_, input_.size()-used_, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { break; }
                os_fail("recovery recv");
            }
            if (n == 0) { throw std::runtime_error("Recovery TCP connection closed"); }
            used_ += static_cast<std::size_t>(n); last_rx_ = now;
            std::size_t at = 0;
            while (used_-at >= 8) {
                const auto size = std::size_t{input_[at]} | (std::size_t{input_[at+1]} << 8);
                if (size < 8) { throw std::runtime_error("Invalid recovery TCP frame length"); }
                if (used_-at < size) { break; }
                const auto frame = std::span<const std::uint8_t>(input_).subspan(at, size);
                if (read32(frame, 4) != 0 || (frame[3] != 0 && frame[3] != unit_)) { throw std::runtime_error("Invalid recovery TCP sequence/unit"); }
                std::size_t offset = 8;
                for (unsigned i = 0; i < frame[2]; ++i) {
                    if (offset+2 > size || frame[offset] < 2 || offset+frame[offset] > size) {
                        throw std::runtime_error("Invalid recovery TCP message framing");
                    }
                    const auto message = frame.subspan(offset, frame[offset]);
                    if (message[1] == 2) {
                        if (message.size() < 3 || message[2] != 'A' || logged_in) { throw std::runtime_error("Recovery login rejected or repeated"); }
                        logged_in = true;
                    } else {
                        if (!logged_in) { throw std::runtime_error("Recovery data before login"); }
                        handle(message);
                    }
                    offset += message.size();
                }
                if (offset != size) { throw std::runtime_error("Trailing recovery TCP bytes"); }
                at += size;
            }
            if (at) { std::memmove(input_.data(), input_.data()+at, used_-at); used_ -= at; }
        }
        if (now-last_rx_ > 10'000'000'000ULL) { throw std::runtime_error("Recovery TCP heartbeat timeout"); }
        if (logged_in && writable() && now-last_tx_ >= 1'000'000'000) { send_message({}); }
    }
private:
    std::uint8_t unit_;
    int fd_ = -1;
    std::array<std::uint8_t, 65535> input_{};
    std::array<std::uint8_t, 64> out_{};
    std::size_t used_ = 0, out_size_ = 0, out_at_ = 0;
    std::uint64_t last_rx_ = 0, last_tx_ = 0;
};
} // namespace

struct Recovery::Impl {
    struct Entry {
        std::array<std::uint8_t, 255> bytes{};
        std::uint8_t length = 0;
        std::uint64_t rx = 0;
    };
    RecoveryOptions options;
    std::uint8_t unit;
    std::unique_ptr<Session> grp, spin;
    std::vector<Entry> entries;
    std::size_t head = 0, buffered = 0;
    std::uint32_t expected = 1, frontier = 1;
    bool seen = false, recovering = false, spinning = false, spin_requested = false, spin_receiving = false;
    bool pending = false, ack = false;
    std::uint32_t request_first = 0, request_end = 0, image = 0, selected_image = 0, order_count = 0, received_orders = 0;
    std::uint16_t request_count = 0;
    std::uint64_t started = 0, deadline = 0, spin_deadline = 0, last_tick = 0;
    std::array<std::uint64_t, 1500> request_times{};
    RecoveryStats stats;

    Impl(const RecoveryOptions& o, std::uint8_t u) : options(o), unit(u), entries(o.buffer_messages) {
        if (!unit || entries.empty() || entries.size() > 1'000'000 || !o.max_gap || !o.timeout_ns) { throw std::invalid_argument("Invalid recovery bounds"); }
        grp = std::make_unique<Session>(o.grp,unit);
        if (!o.spin_server.empty()) { spin = std::make_unique<Session>(o.spin_server,unit); }
    }
    void begin(Processor& p, std::uint64_t now) {
        if (!recovering) { recovering = true; started = now; ++stats.gaps; p.begin_recovery(); }
    }
    void start_spin(Processor& p, std::uint64_t now) {
        if (!spin) { throw std::runtime_error("Gap cannot be recovered; no spin server configured"); }
        if (!spinning) {
            begin(p, now); spinning = true; spin_requested = false; spin_receiving = false;
            spin_deadline = now+options.timeout_ns; ++stats.spins;
        }
    }
    void finish(Processor& p, std::uint64_t now) {
        if (recovering && !spinning && expected == frontier && buffered == 0) {
            recovering = false; p.finish_recovery(); ++stats.completed;
            const auto duration = now-started; stats.total_ns += duration; stats.max_ns = std::max(stats.max_ns, duration);
        }
    }
    void drain(Processor& p, std::uint64_t now) {
        if (spinning) { return; }
        while (entries[head].length) {
            auto& entry = entries[head];
            Packet packet; packet.size = std::size_t{entry.length}+8;
            put(packet.bytes, 0, packet.size, 2); packet.bytes[2] = 1; packet.bytes[3] = unit;
            put(packet.bytes, 4, expected, 4);
            std::copy_n(entry.bytes.begin(), entry.length, packet.bytes.begin()+8);
            p.process(packet.data(), entry.rx, false);
            entry.length = 0; --buffered; head = (head+1)%entries.size(); expected = next(expected);
        }
        finish(p, now);
    }
    void rebase(std::uint32_t sequence) {
        const auto skip = distance(expected, sequence);
        if (skip > UINT32_MAX/2) { throw std::runtime_error("Spin image precedes buffered suffix"); }
        if (skip >= entries.size()) {
            for (auto& e : entries) { e.length = 0; } buffered = 0; head = 0;
        } else {
            for (std::uint32_t i = 0; i < skip; ++i) {
                auto& e = entries[head]; if (e.length) { e.length = 0; --buffered; }
                head = (head+1)%entries.size();
            }
        }
        if (distance(expected, frontier) < skip) { frontier = sequence; }
        expected = sequence;
    }
    void control(Processor& p, std::span<const std::uint8_t> m, bool from_spin, std::uint64_t now) {
        if (!from_spin) {
            if (m[1] != 4 || m.size() < 10 || request_count == 0 || m[2] != unit || read32(m,3) != request_first ||
                (std::uint16_t{m[7]} | (std::uint16_t{m[8]} << 8)) != request_count) {
                throw std::runtime_error("Unexpected GRP response");
            }
            ack = true;
            if (m[9] != 'A') { ++stats.rejections; pending = false; if (!spinning) { start_spin(p, now); } }
            return;
        }
        if (m[1] == 0x80 && m.size() >= 6) { image = read32(m,2); return; }
        if (m[1] == 0x82 && m.size() >= 11 && spin_requested && !spin_receiving) {
            if (m[10] != 'A') { throw std::runtime_error("Spin request rejected"); }
            selected_image = read32(m,2);
            if (!selected_image) { throw std::runtime_error("Unsequenced spin image boundary"); }
            rebase(next(selected_image)); p.begin_spin();
            order_count = read32(m,6); received_orders = 0; spin_receiving = true; return;
        }
        if (m[1] == 0x83 && m.size() >= 6 && spin_receiving) {
            if (read32(m,2) != selected_image || received_orders != order_count) { throw std::runtime_error("Incomplete spin image"); }
            p.finish_spin(selected_image); spinning = false; spin_requested = false; spin_receiving = false;
            drain(p, now); return;
        }
        if (!spin_receiving) { throw std::runtime_error("Unexpected spin server message"); }
        p.spin_message(m);
        if (m[1] == 0x21 || m[1] == 0x22 || m[1] == 0x2f) { ++received_orders; }
    }
};

Recovery::Recovery(const RecoveryOptions& o, std::uint8_t unit) : impl_(std::make_unique<Impl>(o, unit)) {}
Recovery::~Recovery() = default;
bool Recovery::active() const { return impl_->recovering; }
const RecoveryStats& Recovery::stats() const { return impl_->stats; }
void Recovery::note_loss(Processor& p, std::uint64_t now) { impl_->begin(p, now); }
void Recovery::receive(Processor& p, std::span<std::uint8_t> payload, std::uint64_t rx, bool replay) {
    auto& s = *impl_;
    const auto now = monotonic_ns();
    validate_pitch(payload);
    if (payload[3] != s.unit) {
        if (payload[3] == 0 && payload[2] == 0 && read32(payload,4) == 0) { return; }
        if (!replay) { p.process(payload, rx); } return;
    }
    const auto sequence = read32(payload,4);
    if (!sequence) {
        if (payload[2]) { throw std::runtime_error("Unsequenced multicast data in recovery"); }
        return;
    }
    if (replay && !s.recovering) { return; }
    if (!replay && !s.seen && sequence != 1) {
        s.expected = sequence; s.frontier = sequence; s.start_spin(p, now);
    }
    if (!replay) { s.seen = true; }
    if (!replay && !s.recovering && sequence == s.expected) {
        p.process(payload, rx);
        s.expected = advance_sequence(sequence, payload[2]); s.frontier = s.expected; return;
    }
    const auto new_hole = distance(s.frontier, sequence);
    auto end = advance_sequence(sequence, payload[2]);
    if (!replay && distance(s.expected,end) <= UINT32_MAX/2 &&
        distance(s.expected,end) > distance(s.expected,s.frontier)) { s.frontier = end; }
    if (!replay && distance(s.expected,sequence) > 0 && distance(s.expected,sequence) <= UINT32_MAX/2) {
        s.begin(p, now);
        if (new_hole <= UINT32_MAX/2 && new_hole > s.options.max_gap) { s.start_spin(p, now); }
    }
    // A whole datagram is validated before it can contribute buffered messages.
    std::size_t offset = 8;
    for (unsigned i = 0; i < payload[2]; ++i) {
        const auto m = payload.subspan(offset, payload[offset]); (void)decode_pitch_message(m);
        const auto seq = next(sequence,i); auto d = distance(s.expected,seq);
        if (d > UINT32_MAX/2) { ++s.stats.duplicates; offset += m.size(); continue; }
        if (d >= s.entries.size()) {
            if (s.spinning) { throw std::runtime_error("Recovery buffer exhausted during spin"); }
            ++s.stats.buffer_overflows; s.start_spin(p, now); s.rebase(seq); d = 0;
        }
        auto& entry = s.entries[(s.head+d)%s.entries.size()];
        if (entry.length) {
            if (entry.length != m.size() || !std::equal(m.begin(),m.end(),entry.bytes.begin())) { throw std::runtime_error("Conflicting duplicate PITCH message"); }
            ++s.stats.duplicates;
        } else {
            std::copy(m.begin(),m.end(),entry.bytes.begin()); entry.length = static_cast<std::uint8_t>(m.size()); entry.rx = rx;
            ++s.buffered; s.stats.buffered_high_watermark = std::max(s.stats.buffered_high_watermark, std::uint64_t{s.buffered});
            if (replay) { ++s.stats.replay_messages; }
        }
        offset += m.size();
    }
    s.drain(p, now);
}
void Recovery::tick(Processor& p, std::uint64_t now) {
    auto& s = *impl_;
    if (now-s.last_tick < 100'000) { return; } s.last_tick = now;
    if (s.grp) {
        // Malformed protocol and failed TCP sessions terminate ingestion.
        s.grp->tick(now,[&](auto m) { s.control(p,m,false,now); });
    }
    if (s.spin) { s.spin->tick(now,[&](auto m) { s.control(p,m,true,now); }); }
    if (s.pending && s.ack && distance(s.request_end,s.expected) <= UINT32_MAX/2) { s.pending = false; }
    if (s.pending && now >= s.deadline) { ++s.stats.timeouts; s.pending = false; s.start_spin(p,now); }
    if (s.spinning) {
        if (now >= s.spin_deadline) { throw std::runtime_error("Spin recovery timeout"); }
        if (!s.spin_requested && s.spin->logged_in && s.spin->writable() && s.image &&
            distance(s.expected,next(s.image)) <= UINT32_MAX/2) {
            std::array<std::uint8_t,6> request{6,0x81}; put(request,2,s.image,4);
            s.spin->send_message(request); s.spin_requested = true;
        }
        return;
    }
    if (!s.recovering || s.expected == s.frontier || s.pending || !s.grp->logged_in || !s.grp->writable()) { return; }
    if (s.stats.requests >= 100000) { s.start_spin(p,now); return; }
    const auto n = s.stats.requests;
    if ((n >= 320 && now-s.request_times[(n-320)%1500] < 1'000'000'000) ||
        (n >= 1500 && now-s.request_times[n%1500] < 60'000'000'000ULL)) { return; }
    std::uint16_t count = 0;
    const auto available = std::min<std::size_t>({100, distance(s.expected,s.frontier), s.entries.size()});
    while (count < available && !s.entries[(s.head+count)%s.entries.size()].length) { ++count; }
    if (!count) { return; }
    std::array<std::uint8_t,9> request{9,3}; request[2] = s.unit; put(request,3,s.expected,4); put(request,7,count,2);
    s.grp->send_message(request); s.pending = true; s.ack = false;
    s.request_first = s.expected; s.request_count = count; s.request_end = next(s.expected,count);
    s.deadline = now+s.options.timeout_ns; s.request_times[n%1500] = now; ++s.stats.requests;
}
} // namespace low_latency
