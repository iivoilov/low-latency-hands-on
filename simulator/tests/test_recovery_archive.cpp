#include "low_latency/recovery_archive.hpp"
#include "low_latency/processor.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void check(bool ok,const char* message) { if (!ok) { throw std::runtime_error(message); } }
void put(std::span<std::uint8_t> b,std::size_t at,std::uint64_t v,unsigned size) {
    for (unsigned i=0;i<size;++i) { b[at+i]=static_cast<std::uint8_t>(v>>(i*8)); }
}
}
int main() {
    const auto path=std::filesystem::temp_directory_path()/("pitch-archive-"+std::to_string(getpid())+".pcap");
    try {
        low_latency::PitchGenerator generator(73,1,8,{"TEST"},3,2);
        std::vector<low_latency::Packet> packets;
        {
            low_latency::PcapWriter writer(path.string(),"127.0.0.1","239.255.0.1",30001);
            for (unsigned i=0;i<80;++i) {
                auto packet=generator.next(std::uint64_t{i}*100'000'000);
                writer.write(packet,std::uint64_t{i}*100'000'000); packets.push_back(packet);
            }
            writer.finish();
        }
        const low_latency::RecoveryArchive archive(path.string(),1,17);
        check(archive.checkpoints()==archive.messages()/17+1,"Checkpoint count");
        check(archive.through_time(0)==archive.packets().front().through,"Time lookup first packet");
        check(archive.through_time(UINT64_MAX)==archive.messages(),"Time lookup end");
        low_latency::Processor reference(1,0,10000,low_latency::LossPolicy::stop,low_latency::monotonic_ns,16,100,1000);
        std::size_t through=0;
        for (const auto& source:packets) {
            std::size_t offset=8;
            for (unsigned i=0;i<source.bytes[2];++i) {
                low_latency::Packet packet; packet.size=std::size_t{source.bytes[offset]}+8;
                put(packet.bytes,0,packet.size,2); packet.bytes[2]=1; packet.bytes[3]=1;
                put(packet.bytes,4,archive.sequence(through+1),4);
                std::copy_n(source.bytes.begin()+static_cast<std::ptrdiff_t>(offset),source.bytes[offset],packet.bytes.begin()+8);
                reference.process(packet.data(),0,false); ++through;
                low_latency::Processor spun(1,0,1,low_latency::LossPolicy::stop,low_latency::monotonic_ns,16,100,1000);
                spun.begin_spin();
                for (const auto& m:archive.snapshot(through)) { spun.spin_message(m); }
                spun.finish_spin(archive.sequence(through)); spun.finish_recovery();
                // Before the initial Unit Clear, the empty image is still empty.
                check(reference.book().snapshot()==spun.book().snapshot(),"Checkpoint image differs from full replay");
                check(!spun.l1().trade_history_current(),"Spin invented execution history");
                check(spun.time()==reference.time(),"Spin lost final clock context");
                check(archive.find(archive.sequence(through))==through-1,"Sequence index");
                offset+=source.bytes[offset];
            }
        }
        check(reference.book().snapshot()==generator.book(),"Independent replay differs from generator");
        bool invalidated=false;
        reference.set_l1_sink([&](const low_latency::L1Update& e) {
            if (e.changed==63 && !e.bid && !e.ask && !e.last && !e.trade_history_current) { invalidated=true; }
        });
        reference.begin_spin();
        check(invalidated && reference.l1().trades()==0 && !reference.book_current(),"Spin retained old last/quotes");
        for (const auto& m:archive.snapshot(through)) { reference.spin_message(m); }
        reference.finish_spin(archive.sequence(through)); reference.finish_recovery();
        check(reference.book_current() && reference.loss_seen() && !reference.l1().trade_history_current(),"Spin validity");
        check(reference.latency().samples==0,"Snapshot/catch-up entered latency samples");
        check(!archive.find(0),"Sequence zero indexed");
        // Modify bit 1 means Maintain Priority; it must not leak into the
        // reserved bits of a snapshot Add. Preserve the original Add fields.
        const auto first=packets.front();
        std::size_t offset=8; low_latency::PitchMessage initial;
        for (unsigned i=0;i<first.bytes[2];++i) {
            const auto m=low_latency::decode_pitch_message(first.data().subspan(offset,first.bytes[offset]));
            if (m.type==0x21) { initial=m; break; } offset+=first.bytes[offset];
        }
        check(initial.type==0x21,"Missing synthetic initial Add");
        low_latency::Packet modify; modify.size=35;
        put(modify.bytes,0,35,2); modify.bytes[2]=1; modify.bytes[3]=1;
        put(modify.bytes,4,low_latency::advance_sequence(1,first.bytes[2]),4);
        modify.bytes[8]=27; modify.bytes[9]=0x27;
        put(modify.bytes,14,initial.order_id,8); put(modify.bytes,22,42,4);
        put(modify.bytes,26,100001,8); modify.bytes[34]=3;
        {
            low_latency::PcapWriter writer(path.string(),"127.0.0.1","239.255.0.1",30001);
            writer.write(first,0); writer.write(modify,1); writer.finish();
        }
        const low_latency::RecoveryArchive modified(path.string(),1,2);
        bool checked=false;
        for (const auto& bytes:modified.snapshot(modified.messages())) {
            const auto m=low_latency::decode_pitch_message(bytes);
            if (m.type==0x2f && m.order_id==initial.order_id) {
                check(m.quantity==42 && m.price==100001 && m.flags==initial.flags,"Incorrect snapshot Add after Modify");
                checked=true;
            }
        }
        check(checked,"Modified order missing from image");
        std::filesystem::remove(path);
        std::cout << "archive: every message boundary matches independent full replay\n";
    } catch (const std::exception& e) { std::filesystem::remove(path); std::cerr << e.what() << '\n'; return 1; }
}
