#include "low_latency/recovery_archive.hpp"
#include <algorithm>
#include <stdexcept>

namespace low_latency {
namespace {
std::uint32_t read_sequence(const Packet& p) {
    std::uint32_t n=0;
    for (unsigned i=0;i<4;++i) { n |= std::uint32_t{p.bytes[4+i]} << (i*8); }
    return n;
}
void put(std::span<std::uint8_t> b, std::size_t at, std::uint64_t v, unsigned size) {
    for (unsigned i=0;i<size;++i) { b[at+i]=static_cast<std::uint8_t>(v>>(i*8)); }
}
RecoveryArchive::Bytes add_message(const PitchMessage& m) {
    RecoveryArchive::Bytes b(41,0); b[0]=41; b[1]=0x2f;
    put(b,2,m.time_offset,4); put(b,6,m.order_id,8); b[14]=static_cast<std::uint8_t>(m.side);
    put(b,15,m.quantity,4); std::copy(m.symbol.begin(),m.symbol.end(),b.begin()+19);
    put(b,27,m.price,8); b[35]=m.flags;
    std::copy(m.participant.begin(),m.participant.end(),b.begin()+36); b[40]=static_cast<std::uint8_t>(m.customer);
    return b;
}
} // namespace
RecoveryArchive::RecoveryArchive(const std::string& path,std::uint8_t unit,std::size_t every)
    : unit_(unit),checkpoint_every_(every) {
    if (!unit || !every) { throw std::invalid_argument("Need nonzero archive unit/checkpoint interval"); }
    PcapReader reader(path); Packet packet; State state;
    checkpoints_.push_back(state);
    std::uint64_t origin=0; std::uint32_t expected=1;
    while (reader.next(packet)) {
        const auto timestamp=reader.timestamp_ns();
        if (packets_.empty()) { origin=timestamp; }
        if (timestamp<origin || (!packets_.empty() && timestamp-origin<packets_.back().time_ns) || timestamp-origin>=86'400'000'000'000ULL) {
            throw std::runtime_error("Recovery PCAP times must be nondecreasing and shorter than one day");
        }
        const auto seq=read_sequence(packet);
        if (packet.bytes[3]!=unit && !(seq==0 && packet.bytes[2]==0 && packet.bytes[3]==0)) {
            throw std::runtime_error("Recovery archive supports exactly one PITCH unit");
        }
        if (seq==0) {
            if (packet.bytes[2]!=0) { throw std::runtime_error("Unsequenced market data in recovery archive"); }
        } else {
            if (seq!=expected) { throw std::runtime_error("Recovery archive requires a complete PCAP starting at sequence one"); }
            std::size_t offset=8;
            for (unsigned i=0;i<packet.bytes[2];++i) {
                const auto m=packet.data().subspan(offset,packet.bytes[offset]);
                if (!index_.emplace(expected,messages_.size()).second) { throw std::runtime_error("Repeated sequence epoch in recovery archive"); }
                messages_.emplace_back(m.begin(),m.end()); sequences_.push_back(expected);
                apply(state,messages_.size()-1);
                if (messages_.size()%checkpoint_every_==0) { checkpoints_.push_back(state); }
                expected=advance_sequence(expected,1); offset+=m.size();
            }
        }
        packets_.push_back({timestamp-origin,packet,messages_.size()});
    }
    if (packets_.empty() || messages_.empty()) { throw std::runtime_error("Empty recovery archive"); }
}
void RecoveryArchive::apply(State& s,std::size_t index) const {
    const auto& bytes=messages_[index]; const auto m=decode_pitch_message(bytes);
    if (m.type==0x20) { s.time=index; return; }
    if (m.type==0xb1) { s.reference=index; return; }
    if (m.type==0x97) { s.orders.clear(); s.initialized=true; return; }
    if (m.order_update()) {
        if (!s.initialized) { throw std::runtime_error("Recovery PCAP needs initial Unit Clear"); }
        if (m.type==0x21 || m.type==0x22 || m.type==0x2f) {
            if (!s.orders.emplace(m.order_id,Stored{m,{},index,s.time,s.reference}).second) { throw std::runtime_error("Duplicate archive order"); }
        } else {
            const auto found=s.orders.find(m.order_id);
            if (found==s.orders.end()) { throw std::runtime_error("Missing archive order"); }
            auto& saved=found->second; auto& order=saved.order;
            if (m.type==0x29) { s.orders.erase(found); return; }
            if (m.type==0x27 || m.type==0x28) { order.quantity=m.quantity; order.price=m.price; /* Modify flags are not Add flags. */ }
            else if (m.type==0x24) { order.quantity=m.remaining_quantity; }
            else {
                if (m.quantity>order.quantity) { throw std::runtime_error("Archive quantity overdraw"); }
                order.quantity-=m.quantity;
            }
            if (order.quantity==0) { s.orders.erase(found); }
            else { order.time_offset=m.time_offset; saved.index=index; saved.time=s.time; saved.reference=s.reference; }
        }
    } else if (m.type==0x31 || m.type==0x95 || m.type==0x98) {
        const auto key=std::make_pair(m.symbol,m.type);
        if (m.type==0x31 && m.indicator=='S') { s.metadata.erase(key); }
        else { s.metadata[key]=Stored{{},bytes,index,s.time,s.reference}; }
    }
}
std::uint32_t RecoveryArchive::sequence(std::size_t through) const {
    if (!through || through>messages_.size()) { throw std::out_of_range("No spin image at this boundary"); }
    return sequences_[through-1];
}
std::optional<std::size_t> RecoveryArchive::find(std::uint32_t seq) const {
    const auto found=index_.find(seq); if (found==index_.end()) { return std::nullopt; } return found->second;
}
std::size_t RecoveryArchive::through_time(std::uint64_t ns) const {
    const auto found=std::upper_bound(packets_.begin(),packets_.end(),ns,[](auto t,const auto& p){return t<p.time_ns;});
    return found==packets_.begin()?0:(found-1)->through;
}
std::vector<RecoveryArchive::Bytes> RecoveryArchive::snapshot(std::size_t through) const {
    if (through>messages_.size()) { throw std::out_of_range("Snapshot beyond PCAP"); }
    const auto checkpoint=through/checkpoint_every_;
    State state=checkpoints_.at(checkpoint);
    for (auto i=checkpoint*checkpoint_every_;i<through;++i) { apply(state,i); }
    std::vector<const Stored*> sorted; sorted.reserve(state.orders.size()+state.metadata.size());
    for (const auto& [id,value]:state.orders) { sorted.push_back(&value); }
    for (const auto& [id,value]:state.metadata) { sorted.push_back(&value); }
    std::sort(sorted.begin(),sorted.end(),[](auto a,auto b){return a->index<b->index;});
    std::vector<Bytes> result;
    auto time=absent,reference=absent;
    const auto context=[&](std::size_t r,std::size_t t) {
        if (r!=absent && r!=reference) { result.push_back(messages_[r]); reference=r; }
        if (t!=absent && t!=time) { result.push_back(messages_[t]); time=t; }
    };
    for (const auto* record:sorted) {
        context(record->reference,record->time);
        result.push_back(record->metadata.empty()?add_message(record->order):record->metadata);
    }
    context(state.reference,state.time);
    return result;
}
} // namespace low_latency
