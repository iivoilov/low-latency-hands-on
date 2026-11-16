#include "cli.hpp"
#include "low_latency/recovery_archive.hpp"
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>

namespace {
using namespace low_latency;
using Clock=std::chrono::steady_clock;
std::atomic<bool> stopped{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void stop(int) { stopped.store(true,std::memory_order_relaxed); }
std::uint64_t now_ns() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }
[[noreturn]] void fail(const char* op) { throw std::system_error(errno,std::generic_category(),op); }
struct Fd {
    int value;
    explicit Fd(int v):value(v) { if (v<0) { fail("socket"); } }
    ~Fd() { close(value); }
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
};
void put(std::span<std::uint8_t> b,std::size_t at,std::uint64_t v,unsigned size) {
    for (unsigned i=0;i<size;++i) { b[at+i]=static_cast<std::uint8_t>(v>>(i*8)); }
}
std::uint32_t read32(std::span<const std::uint8_t> b,std::size_t at) {
    std::uint32_t v=0; for (unsigned i=0;i<4;++i) { v |= std::uint32_t{b[at+i]}<<(i*8); } return v;
}
struct Options {
    std::string input,group="239.255.0.1",gap_group="239.255.0.2",interface="127.0.0.1",listen="127.0.0.1",drop_packets;
    std::uint16_t port=30001,gap_port=30002,grp_port=30003,spin_port=30004;
    std::uint8_t unit=1;
    std::size_t checkpoint=4096,fragment=65535;
    unsigned start_delay=1000;
    bool replay_before_ack=false,drop_replay_once=false,reject_grp=false;
};
Options parse(int argc,char** argv) {
    Options o;
    for (int i=1;i<argc;++i) {
        const std::string name=argv[i];
        if (name=="--help") {
            std::cout << "Usage: serve_feed FILE.pcap [options]\n"
                "  --group IPv4 / --port N          Live multicast (239.255.0.1:30001)\n"
                "  --gap-group IPv4 / --gap-port N  Replay multicast (239.255.0.2:30002)\n"
                "  --interface IPv4                Sender interface (127.0.0.1)\n"
                "  --listen IPv4                   TCP service address (127.0.0.1)\n"
                "  --grp-port N / --spin-port N     TCP ports (30003/30004; 0 chooses ephemeral)\n"
                "  --unit N                        Single equities unit (1)\n"
                "  --checkpoint-messages N         Snapshot replay bound (4096)\n"
                "  --start-delay-ms N               Delay after READY (1000)\n"
                "  --drop-packets LIST             1-based live datagrams, e.g. 10,20-25\n"
                "  --replay-before-ack              Deliver UDP replay before acknowledgment\n"
                "  --drop-replay-once               Omit the first replay datagram\n"
                "  --reject-grp                     Reject requests as out of range\n"
                "  --tcp-fragment-bytes N           Limit each TCP write (65535)\n"
                "Login fields: PITCH_SESSION (4), PITCH_USERNAME (4), PITCH_PASSWORD (10).\n"
                "Preloads and indexes PCAP; serves until SIGINT/SIGTERM, including after playback.\n";
            std::exit(0);
        }
        if (name=="--replay-before-ack") { o.replay_before_ack=true; continue; }
        if (name=="--drop-replay-once") { o.drop_replay_once=true; continue; }
        if (name=="--reject-grp") { o.reject_grp=true; continue; }
        if (!name.starts_with("--") && o.input.empty()) { o.input=name; continue; }
        if (i+1==argc) { throw std::invalid_argument("Missing option value"); }
        const std::string value=argv[++i];
        using cli::integer;
        if (name=="--group") { o.group=value; }
        else if (name=="--gap-group") { o.gap_group=value; }
        else if (name=="--interface") { o.interface=value; }
        else if (name=="--listen") { o.listen=value; }
        else if (name=="--port") { o.port=static_cast<std::uint16_t>(integer(name,value,65535)); }
        else if (name=="--gap-port") { o.gap_port=static_cast<std::uint16_t>(integer(name,value,65535)); }
        else if (name=="--grp-port") { o.grp_port=static_cast<std::uint16_t>(integer(name,value,65535)); }
        else if (name=="--spin-port") { o.spin_port=static_cast<std::uint16_t>(integer(name,value,65535)); }
        else if (name=="--unit") { o.unit=static_cast<std::uint8_t>(integer(name,value,255)); }
        else if (name=="--checkpoint-messages") { o.checkpoint=static_cast<std::size_t>(integer(name,value,1000000)); }
        else if (name=="--start-delay-ms") { o.start_delay=static_cast<unsigned>(integer(name,value,60000)); }
        else if (name=="--tcp-fragment-bytes") { o.fragment=static_cast<std::size_t>(integer(name,value,65535)); }
        else if (name=="--drop-packets") { o.drop_packets=value; }
        else { throw std::invalid_argument("Unknown option: "+name); }
    }
    if (o.input.empty() || !o.unit || !o.checkpoint || !o.fragment || !o.port || !o.gap_port ||
        !IN_MULTICAST(ntohl(cli::address(o.group).s_addr)) || !IN_MULTICAST(ntohl(cli::address(o.gap_group).s_addr)) ||
        (o.group==o.gap_group && o.port==o.gap_port)) { throw std::invalid_argument("Invalid recovery simulator configuration"); }
    cli::address(o.interface); cli::address(o.listen);
    return o;
}
struct Listener {
    Fd fd{socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0)};
    std::uint16_t port;
    Listener(const std::string& ip,std::uint16_t p):port(p) {
        const int yes=1; setsockopt(fd.value,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        sockaddr_in local{}; local.sin_family=AF_INET; local.sin_addr=cli::address(ip); local.sin_port=htons(p);
        if (bind(fd.value,reinterpret_cast<sockaddr*>(&local),sizeof(local))<0 || listen(fd.value,8)<0) { fail("listen"); }
        socklen_t size=sizeof(local);
        if (getsockname(fd.value,reinterpret_cast<sockaddr*>(&local),&size)<0) { fail("getsockname"); }
        port=ntohs(local.sin_port);
    }
};
class Sender {
    Fd fd_{socket(AF_INET,SOCK_DGRAM,0)};
    sockaddr_in target_{};
public:
    Sender(const Options& o,bool replay) {
        const auto interface=cli::address(o.interface); const unsigned char one=1;
        if (setsockopt(fd_.value,IPPROTO_IP,IP_MULTICAST_IF,&interface,sizeof(interface))<0 ||
            setsockopt(fd_.value,IPPROTO_IP,IP_MULTICAST_TTL,&one,sizeof(one))<0 ||
            setsockopt(fd_.value,IPPROTO_IP,IP_MULTICAST_LOOP,&one,sizeof(one))<0) { fail("multicast setup"); }
        target_.sin_family=AF_INET; target_.sin_addr=cli::address(replay?o.gap_group:o.group); target_.sin_port=htons(replay?o.gap_port:o.port);
    }
    void send(const Packet& p) {
        const auto n=sendto(fd_.value,p.bytes.data(),p.size,0,reinterpret_cast<sockaddr*>(&target_),sizeof(target_));
        if (n<0 || static_cast<std::size_t>(n)!=p.size) { fail("sendto"); }
    }
};
struct Peer {
    Fd fd;
    std::size_t fragment;
    std::vector<std::uint8_t> input;
    std::uint64_t last_rx=now_ns(),last_tx=now_ns();
    Peer(int raw,std::size_t f):fd(raw),fragment(f) {
        input.reserve(65535); const timeval timeout{1,0};
        if (setsockopt(raw,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))<0) { fail("TCP send timeout"); }
    }
    void send(std::span<const std::uint8_t> message) {
        std::vector<std::uint8_t> frame(message.size()+8,0); put(frame,0,frame.size(),2); frame[2]=message.empty()?0:1;
        std::copy(message.begin(),message.end(),frame.begin()+8);
        std::size_t at=0;
        while (at<frame.size()) {
            const auto n=::send(fd.value,frame.data()+at,std::min(fragment,frame.size()-at),MSG_NOSIGNAL);
            if (n<0 && errno==EINTR && !stopped) { continue; }
            if (n<=0) { throw std::runtime_error("Simulator TCP send failed"); }
            at+=static_cast<std::size_t>(n);
        }
        last_tx=now_ns();
    }
    std::optional<std::vector<std::uint8_t>> receive() {
        if (input.size()>=8) {
            const auto size=std::size_t{input[0]}|(std::size_t{input[1]}<<8);
            if (size<8) { throw std::runtime_error("Invalid client TCP length"); }
            if (input.size()>=size) {
                std::vector<std::uint8_t> result(input.begin(),input.begin()+static_cast<std::ptrdiff_t>(size));
                input.erase(input.begin(),input.begin()+static_cast<std::ptrdiff_t>(size)); return result;
            }
        }
        pollfd p{fd.value,POLLIN,0}; const auto ready=poll(&p,1,20);
        if (ready<0 && errno!=EINTR) { fail("TCP poll"); }
        if (ready<=0) { return std::nullopt; }
        std::array<std::uint8_t,4096> bytes{};
        const auto n=recv(fd.value,bytes.data(),std::min(bytes.size(),65535-input.size()),0);
        if (n<=0) { throw std::runtime_error("Simulator client disconnected"); }
        input.insert(input.end(),bytes.begin(),bytes.begin()+n); last_rx=now_ns(); return std::nullopt;
    }
};
struct Login {
    std::array<std::uint8_t,22> bytes{};
    Login() {
        bytes.fill(' '); bytes[0]=22; bytes[1]=1;
        for (const auto& [name,at,size]:{std::tuple{"PITCH_SESSION",2U,4U},std::tuple{"PITCH_USERNAME",6U,4U},std::tuple{"PITCH_PASSWORD",12U,10U}}) {
            const char* v=std::getenv(name);
            if (!v || std::strlen(v)>size) { throw std::invalid_argument(std::string("Set ")+name+" to a valid login field"); }
            std::copy_n(v,std::strlen(v),bytes.begin()+at);
        }
    }
};
void serve_peer(int raw,bool spin,const Options& o,const Login& login,const RecoveryArchive& archive,
                const std::atomic<std::size_t>& published,std::atomic<bool>& replay_drop,std::atomic<bool>& occupied) {
    Peer peer(raw,o.fragment); bool authenticated=false,owns_session=false;
    try {
        Sender replay(o,true);
        std::deque<std::pair<std::uint32_t,std::size_t>> images;
        std::deque<std::uint64_t> requests;
        std::deque<std::vector<std::uint8_t>> controls;
        std::uint64_t last_image=0,total_requests=0;
        std::uint32_t future_request=0;
        const auto send_spin=[&](std::uint32_t requested) {
            std::optional<std::pair<std::uint32_t,std::size_t>> chosen;
            for (const auto& im:images) {
                const auto d=im.first>=requested?im.first-requested:UINT32_MAX-requested+im.first;
                if (d<=UINT32_MAX/2) { chosen=im; break; }
            }
            std::vector<RecoveryArchive::Bytes> image;
            std::uint32_t count=0;
            if (chosen) {
                image=archive.snapshot(chosen->second);
                for (const auto& m:image) { if (m[1]==0x21 || m[1]==0x22 || m[1]==0x2f) { ++count; } }
            }
            std::array<std::uint8_t,11> response{11,0x82}; put(response,2,chosen?chosen->first:requested,4);
            put(response,6,count,4); response[10]=chosen?'A':'O'; peer.send(response);
            if (chosen) {
                for (const auto& m:image) { peer.send(m); }
                std::array<std::uint8_t,6> finished{6,0x83}; put(finished,2,chosen->first,4); peer.send(finished);
            }
        };
        while (!stopped) {
            const auto now=now_ns();
            if (now-peer.last_rx>10'000'000'000ULL) { throw std::runtime_error("Simulator client heartbeat timeout"); }
            if (authenticated && now-peer.last_tx>=1'000'000'000) { peer.send({}); }
            if (authenticated && spin && now-last_image>=1'000'000'000) {
                const auto through=published.load(std::memory_order_acquire);
                if (through) {
                    const auto seq=archive.sequence(through); images.emplace_back(seq,through);
                    if (images.size()>10) { images.pop_front(); }
                    std::array<std::uint8_t,6> available{6,0x80}; put(available,2,seq,4); peer.send(available);
                    last_image=now;
                    if (future_request) { send_spin(future_request); future_request=0; }
                }
            }
            std::optional<std::vector<std::uint8_t>> block;
            if (controls.empty()) {
                block=peer.receive(); if (!block) { continue; }
                if ((*block)[2]>1) {
                    const auto& frame=*block;
                    std::size_t at=8;
                    for (unsigned i=0;i<frame[2];++i) {
                        if (at+2>frame.size() || frame[at]<2 || at+frame[at]>frame.size()) {
                            throw std::runtime_error("Invalid client multi-message framing");
                        }
                        std::vector<std::uint8_t> single(frame.begin(),frame.begin()+8);
                        single.insert(single.end(),frame.begin()+static_cast<std::ptrdiff_t>(at),frame.begin()+static_cast<std::ptrdiff_t>(at+frame[at]));
                        single[2]=1; put(single,0,single.size(),2); controls.push_back(std::move(single));
                        at+=frame[at];
                    }
                    if (at!=frame.size()) { throw std::runtime_error("Trailing client TCP bytes"); }
                    continue;
                }
            } else { block=std::move(controls.front()); controls.pop_front(); }
            const auto& b=*block;
            if (read32(b,4)!=0 || b[3]!=0) { throw std::runtime_error("Invalid client control header"); }
            if (b[2]==0 && b.size()==8) { if (!authenticated) { throw std::runtime_error("Client heartbeat before login"); } continue; }
            if (b[2]!=1 || b.size()<10 || b[8]!=b.size()-8) { throw std::runtime_error("Invalid client control message"); }
            const auto m=std::span<const std::uint8_t>(b).subspan(8);
            if (!authenticated) {
                if (m.size()!=22 || m[1]!=1) { throw std::runtime_error("Client must log in first"); }
                char status='N';
                if (std::equal(m.begin(),m.end(),login.bytes.begin())) {
                    owns_session=!occupied.exchange(true); status=owns_session?'A':'B';
                }
                const std::array<std::uint8_t,3> response{3,2,static_cast<std::uint8_t>(status)}; peer.send(response);
                if (status!='A') { break; } authenticated=true; continue;
            }
            if (spin) {
                if (m[1]!=0x81 || m.size()<6) { throw std::runtime_error("Unsupported spin request"); }
                const auto requested=read32(m,2);
                if (images.empty() || (requested!=images.back().first &&
                    (requested>images.back().first?requested-images.back().first:UINT32_MAX-images.back().first+requested)<=UINT32_MAX/2)) {
                    future_request=requested;
                } else { send_spin(requested); }
                continue;
            }
            if (m[1]!=3 || m.size()<9) { throw std::runtime_error("Unsupported gap request"); }
            const auto seq=read32(m,3); const auto count=std::uint16_t{m[7]}|(std::uint16_t{m[8]}<<8);
            const auto through=published.load(std::memory_order_acquire); const auto index=archive.find(seq);
            char status='A';
            while (!requests.empty() && now-requests.front()>=60'000'000'000ULL) { requests.pop_front(); }
            const auto second=std::count_if(requests.begin(),requests.end(),[&](auto t){return now-t<1'000'000'000;});
            if (m[2]!=o.unit) { status='I'; }
            else if (!count || count>100) { status='C'; }
            else if (o.reject_grp || !index || *index+static_cast<unsigned>(count)>through || through-*index>1000000) { status='O'; }
            else if (total_requests>=100000) { status='D'; }
            else if (requests.size()>=1500) { status='M'; }
            else if (second>=320) { status='S'; }
            if (total_requests<100000) { ++total_requests; }
            if (requests.size()<1500) { requests.push_back(now); }
            std::array<std::uint8_t,10> response{10,4,m[2]}; put(response,3,seq,4); put(response,7,static_cast<unsigned>(count),2); response[9]=static_cast<std::uint8_t>(status);
            const auto replay_range=[&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::size_t at=*index,end=*index+static_cast<unsigned>(count);
                while (at<end) {
                    Packet p; p.size=8; p.bytes[3]=o.unit; put(p.bytes,4,archive.sequence(at+1),4);
                    while (at<end && p.size+archive.message(at).size()<=max_payload && p.bytes[2]<255) {
                        const auto& bytes=archive.message(at++); std::copy(bytes.begin(),bytes.end(),p.bytes.begin()+static_cast<std::ptrdiff_t>(p.size));
                        p.size+=bytes.size(); ++p.bytes[2];
                    }
                    put(p.bytes,0,p.size,2);
                    if (!replay_drop.exchange(false)) { replay.send(p); }
                }
            };
            if (status=='A' && o.replay_before_ack) { replay_range(); }
            peer.send(response);
            if (status=='A' && !o.replay_before_ack) { replay_range(); }
        }
    } catch (const std::exception& e) {
        if (!stopped) { std::cerr << "serve_feed session: " << e.what() << '\n'; }
    }
    if (owns_session) { occupied.store(false); }
}
} // namespace
int main(int argc,char** argv) {
    try {
        const auto o=parse(argc,argv); const Login login;
        std::signal(SIGINT,stop); std::signal(SIGTERM,stop);
        const RecoveryArchive archive(o.input,o.unit,o.checkpoint);
        std::vector<bool> drop(archive.packets().size(),false);
        if (!o.drop_packets.empty()) {
            for (const auto& range:cli::symbols(o.drop_packets)) {
                const auto dash=range.find('-');
                const auto first=cli::integer("drop packet",range.substr(0,dash),drop.size());
                const auto last=dash==std::string::npos?first:cli::integer("drop packet",range.substr(dash+1),drop.size());
                if (!first || last<first) { throw std::invalid_argument("Invalid drop range"); }
                for (auto i=first;i<=last;++i) { drop[i-1]=true; }
            }
        }
        Listener grp(o.listen,o.grp_port),spin(o.listen,o.spin_port); Sender live(o,false);
        std::atomic<std::size_t> published{0}; std::atomic<bool> replay_drop{o.drop_replay_once},grp_busy{false},spin_busy{false};
        std::string sender_error;
        std::jthread sender([&](std::stop_token token) {
            try {
                auto start=Clock::now()+std::chrono::milliseconds(o.start_delay);
                std::chrono::nanoseconds slip{}; std::uint64_t sent=0,omitted=0,max_lateness=0;
                for (std::size_t i=0;i<archive.packets().size() && !stopped && !token.stop_requested();++i) {
                    const auto& item=archive.packets()[i]; const auto deadline=start+std::chrono::nanoseconds(item.time_ns)+slip;
                    auto now=Clock::now();
                    while (now<deadline && !stopped && !token.stop_requested()) {
                        if (deadline-now>std::chrono::microseconds(200)) { std::this_thread::sleep_until(std::min(deadline-std::chrono::microseconds(100),now+std::chrono::milliseconds(10))); }
                        now=Clock::now();
                    }
                    if (stopped || token.stop_requested()) { return; }
                    const auto late=std::chrono::duration_cast<std::chrono::nanoseconds>(now-deadline); slip+=late;
                    max_lateness=std::max(max_lateness,static_cast<std::uint64_t>(late.count()));
                    // Publish the exchange frontier even when fault injection omits delivery.
                    published.store(item.through,std::memory_order_release);
                    if (drop[i]) { ++omitted; } else { live.send(item.packet); ++sent; }
                }
                const auto seconds=std::chrono::duration<double>(Clock::now()-start).count();
                std::cout << "stream_finished sent_packets=" << sent << " injected_drops=" << omitted
                    << " elapsed_seconds=" << seconds << " max_lateness_ns=" << max_lateness << " schedule_slip_ns=" << slip.count() << std::endl;
                Packet heartbeat; heartbeat.size=8; put(heartbeat.bytes,0,8,2); heartbeat.bytes[3]=o.unit;
                put(heartbeat.bytes,4,advance_sequence(archive.sequence(archive.messages()),1),4);
                auto next_heartbeat=Clock::now();
                while (!stopped && !token.stop_requested()) {
                    if (Clock::now()>=next_heartbeat) { live.send(heartbeat); next_heartbeat=Clock::now()+std::chrono::seconds(1); }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            } catch (const std::exception& e) { sender_error=e.what(); stopped=1; }
        });
        std::cout << "READY grp_port=" << grp.port << " spin_port=" << spin.port << " loaded_packets=" << archive.packets().size()
            << " messages=" << archive.messages() << " checkpoints=" << archive.checkpoints() << std::endl;
        struct Worker {
            std::shared_ptr<std::atomic<bool>> finished;
            std::jthread thread;
        };
        std::vector<Worker> workers;
        // Also stops workers if the accept loop exits through an exception.
        struct StopWorkers { ~StopWorkers() { stopped.store(true); } } stop_workers;
        while (!stopped) {
            std::erase_if(workers,[](const auto& w){return w.finished->load(std::memory_order_acquire);});
            pollfd fds[2]{{grp.fd.value,POLLIN,0},{spin.fd.value,POLLIN,0}};
            const auto ready=poll(fds,2,50); if (ready<0 && errno!=EINTR) { fail("service poll"); }
            for (unsigned i=0;i<2;++i) {
                if (!(fds[i].revents&POLLIN)) { continue; }
                const auto raw=accept(fds[i].fd,nullptr,nullptr); if (raw<0) { continue; }
                // A laboratory service supports one active session per endpoint.
                // Reject excess connections without accumulating unbounded workers.
                if (workers.size()>=32) { close(raw); continue; }
                auto finished=std::make_shared<std::atomic<bool>>(false);
                workers.push_back({finished,std::jthread([&,raw,i,finished] {
                    serve_peer(raw,i==1,o,login,archive,published,replay_drop,i==1?spin_busy:grp_busy);
                    finished->store(true,std::memory_order_release);
                })});
            }
        }
        sender.request_stop(); sender.join();
        if (!sender_error.empty()) { std::cerr << "serve_feed sender: " << sender_error << '\n'; return 1; }
        return 0;
    } catch (const std::exception& e) { std::cerr << "serve_feed: " << e.what() << '\n'; return 1; }
}
